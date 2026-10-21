#include "mir_internal.h"

/* All edits are private until a whole-module replay and structural check pass.
 * Metadata strings remain pinned by the owned source module. */
typedef struct InlineSymbol {
  AstName module, name;
  uint32_t function;
} InlineSymbol;
typedef struct InlineGraph {
  InlineSymbol *symbols;
  uint32_t *offsets, *edges, *order;
  uint8_t *recursive;
  uint32_t count;
} InlineGraph;

static int inline_name_compare(AstName a, AstName b) {
  if (a.span.length != b.span.length)
    return a.span.length < b.span.length ? -1 : 1;
  return a.span.length ? memcmp(a.text, b.text, a.span.length) : 0;
}

static int inline_symbol_compare(const void *a, const void *b) {
  const InlineSymbol *x = (const InlineSymbol *)a, *y = (const InlineSymbol *)b;
  int module = inline_name_compare(x->module, y->module);
  return module ? module : inline_name_compare(x->name, y->name);
}

static uint32_t inline_resolve(const InlineGraph *g,
                               const AstMirFunction *caller,
                               const AstMirInstructionInfo *call) {
  InlineSymbol key = {call->module.span.length ? call->module
                                               : caller->module_name,
                      call->symbol, 0};
  const InlineSymbol *found = (const InlineSymbol *)bsearch(
      &key, g->symbols, g->count, sizeof(*g->symbols), inline_symbol_compare);
  return found ? found->function : AST_MIR_INVALID_ID;
}

static void inline_graph_destroy(InlineGraph *g) {
  free(g->symbols);
  free(g->offsets);
  free(g->edges);
  free(g->order);
  free(g->recursive);
}

/* Iterative Tarjan: no host recursion, quadratic reachability table or unstable
 * hash iteration. Popped SCCs provide a deterministic bottom-up schedule. */
static int inline_graph_build(InlineGraph *g, AstMirFunction *const *functions,
                              size_t count) {
  memset(g, 0, sizeof(*g));
  if (count >= UINT32_MAX)
    return 0;
  uint32_t n = (uint32_t)count;
  g->count = n;
  size_t size = n ? n : 1u;
  g->symbols = (InlineSymbol *)mir_malloc_array(size, sizeof(*g->symbols));
  g->offsets = (uint32_t *)calloc((size_t)n + 1u, sizeof(*g->offsets));
  g->order = (uint32_t *)mir_malloc_array(size, sizeof(*g->order));
  g->recursive = (uint8_t *)calloc(size, 1u);
  if (!g->symbols || !g->offsets || !g->order || !g->recursive)
    return 0;
  for (uint32_t f = 0; f < n; ++f)
    g->symbols[f] =
        (InlineSymbol){functions[f]->module_name, functions[f]->name, f};
  qsort(g->symbols, n, sizeof(*g->symbols), inline_symbol_compare);
  for (uint32_t f = 1; f < n; ++f)
    if (!inline_symbol_compare(&g->symbols[f - 1u], &g->symbols[f]))
      return 0;
  uint64_t used = 0;
  for (uint32_t f = 0; f < n; ++f) {
    const AstMirFunction *fn = functions[f];
    for (uint32_t i = 0; i < fn->instruction_count; ++i)
      if (fn->instructions[i].info.op == AST_MIR_OP_CALL_DIRECT &&
          inline_resolve(g, fn, &fn->instructions[i].info) !=
              AST_MIR_INVALID_ID)
        used++;
    if (used > UINT32_MAX)
      return 0;
    g->offsets[f + 1u] = (uint32_t)used;
  }
  g->edges =
      (uint32_t *)mir_malloc_array(used ? (size_t)used : 1u, sizeof(*g->edges));
  if (!g->edges)
    return 0;
  for (uint32_t f = 0; f < n; ++f) {
    const AstMirFunction *fn = functions[f];
    uint32_t at = g->offsets[f];
    for (uint32_t i = 0; i < fn->instruction_count; ++i) {
      if (fn->instructions[i].info.op != AST_MIR_OP_CALL_DIRECT)
        continue;
      uint32_t target = inline_resolve(g, fn, &fn->instructions[i].info);
      if (target != AST_MIR_INVALID_ID)
        g->edges[at++] = target;
    }
  }
  uint32_t *index = (uint32_t *)mir_malloc_array(size, sizeof(*index));
  uint32_t *low = (uint32_t *)mir_malloc_array(size, sizeof(*low));
  uint32_t *stack = (uint32_t *)mir_malloc_array(size, sizeof(*stack));
  uint32_t *dfs = (uint32_t *)mir_malloc_array(size, sizeof(*dfs));
  uint32_t *cursor = (uint32_t *)mir_malloc_array(size, sizeof(*cursor));
  uint32_t *component = (uint32_t *)mir_malloc_array(size, sizeof(*component));
  uint32_t *component_size = (uint32_t *)calloc(size, sizeof(*component_size));
  uint8_t *active = (uint8_t *)calloc(size, 1u);
  int ok = index && low && stack && dfs && cursor && component &&
           component_size && active;
  if (!ok)
    goto done;
  for (uint32_t f = 0; f < n; ++f)
    index[f] = AST_MIR_INVALID_ID;
  uint32_t serial = 0, components = 0, order_count = 0, top = 0, depth = 0;
  for (uint32_t root = 0; root < n; ++root) {
    if (index[root] != AST_MIR_INVALID_ID)
      continue;
    dfs[depth++] = root;
    cursor[root] = g->offsets[root];
    index[root] = low[root] = serial++;
    stack[top++] = root;
    active[root] = 1u;
    while (depth) {
      uint32_t v = dfs[depth - 1u];
      if (cursor[v] < g->offsets[v + 1u]) {
        uint32_t w = g->edges[cursor[v]++];
        if (index[w] == AST_MIR_INVALID_ID) {
          dfs[depth++] = w;
          cursor[w] = g->offsets[w];
          index[w] = low[w] = serial++;
          stack[top++] = w;
          active[w] = 1u;
        } else if (active[w] && index[w] < low[v])
          low[v] = index[w];
        continue;
      }
      depth--;
      if (low[v] == index[v]) {
        uint32_t w;
        do {
          w = stack[--top];
          active[w] = 0;
          component[w] = components;
          component_size[components]++;
          g->order[order_count++] = w;
        } while (w != v);
        components++;
      }
      if (depth && low[v] < low[dfs[depth - 1u]])
        low[dfs[depth - 1u]] = low[v];
    }
  }
  for (uint32_t f = 0; f < n; ++f) {
    g->recursive[f] = (uint8_t)(component_size[component[f]] > 1u);
    for (uint32_t e = g->offsets[f]; e < g->offsets[f + 1u]; ++e)
      if (g->edges[e] == f)
        g->recursive[f] = 1u;
  }
done:
  free(index);
  free(low);
  free(stack);
  free(dfs);
  free(cursor);
  free(component);
  free(component_size);
  free(active);
  return ok;
}

static int inline_finish(AstMirFunction *fn) {
  if (!fn->block_count)
    return 1;
  fn->phi_count = 0;
  for (uint32_t v = 0; v < fn->vreg_count; ++v)
    if (fn->vregs[v].info.kind == AST_MIR_VREG_BLOCK_ARGUMENT &&
        (fn->vregs[v].info.flags & AST_MIR_VREG_PHI))
      fn->phi_count++;
  for (uint32_t b = 0; b < fn->block_count; ++b) {
    AstMirBlock *block = &fn->blocks[b];
    block->memory_argument =
        mir_add_memory(fn,
                       b == fn->entry_block ? AST_MIR_MEMORY_ENTRY
                                            : AST_MIR_MEMORY_BLOCK_ARGUMENT,
                       b, 0);
    if (block->memory_argument == AST_MIR_INVALID_ID)
      return 0;
  }
  for (uint32_t b = 0; b < fn->block_count; ++b) {
    AstMirBlock *block = &fn->blocks[b];
    AstMirMemoryId memory = block->memory_argument;
    for (uint32_t j = 0; j < block->instruction_count; ++j) {
      uint32_t id = block->instructions[j];
      AstMirInstruction *inst = &fn->instructions[id];
      inst->info.memory_in = inst->info.memory_out = AST_MIR_INVALID_ID;
      if (mir_instruction_is_effectful(inst)) {
        inst->info.memory_in = memory;
        memory = mir_add_memory(fn, AST_MIR_MEMORY_INSTRUCTION_RESULT, b, id);
        if (memory == AST_MIR_INVALID_ID)
          return 0;
        inst->info.memory_out = memory;
      }
    }
    block->term.info.memory = memory;
  }
  return mir_analyze_cfg(fn) && mir_link_memory_ssa(fn) &&
         mir_analyze_liveness(fn);
}

static AstMirFunction *inline_clone(const AstMirFunction *source) {
  AstMirFunction *fn = mir_clone_function_header(source);
  if (!fn)
    return NULL;
  fn->entry_block = source->entry_block;
  fn->vreg_count = fn->vreg_capacity = source->vreg_count;
  fn->vregs = (AstMirVRegData *)mir_malloc_array(
      source->vreg_count ? source->vreg_count : 1u, sizeof(*fn->vregs));
  if (!fn->vregs)
    goto fail;
  if (source->vreg_count)
    memcpy(fn->vregs, source->vregs,
           (size_t)source->vreg_count * sizeof(*fn->vregs));
  if (source->block_count) {
    fn->blocks =
        (AstMirBlock *)calloc(source->block_count, sizeof(*fn->blocks));
    if (!fn->blocks)
      goto fail;
    fn->block_count = source->block_count;
  }
  for (uint32_t b = 0; b < source->block_count; ++b) {
    const AstMirBlock *old = &source->blocks[b];
    fn->blocks[b].id = b;
    fn->blocks[b].span = old->span;
    fn->blocks[b].hir_argument_count = old->hir_argument_count;
    for (uint32_t a = 0; a < old->argument_count; ++a)
      if (!mir_append_block_argument(fn, b, old->arguments[a],
                                     old->argument_source_locals[a]))
        goto fail;
    for (uint32_t j = 0; j < old->instruction_count; ++j) {
      const AstMirInstruction *inst =
          &source->instructions[old->instructions[j]];
      uint32_t id = mir_emit_instruction(fn, b, *inst, inst->operands,
                                         inst->info.operand_count);
      if (id == AST_MIR_INVALID_ID)
        goto fail;
      if (inst->info.result != AST_MIR_INVALID_ID)
        fn->vregs[inst->info.result].info.definition_index = id;
    }
    AstMirTerminator term = {0};
    term.info = old->term.info;
    if (!mir_set_terminator(fn, b, term, old->term.true_args,
                            old->term.info.true_arg_count, old->term.false_args,
                            old->term.info.false_arg_count))
      goto fail;
  }
  if (!inline_finish(fn))
    goto fail;
  return fn;
fail:
  mir_function_destroy(fn);
  return NULL;
}

static int inline_frame_safe(const AstMirFunction *fn) {
  if (!fn->block_count ||
      (fn->flags & (AST_HIR_FUNCTION_EXTERN | AST_HIR_FUNCTION_DISPATCH |
                    AST_HIR_FUNCTION_NAKED)) ||
      (fn->frame_policy != AST_HIR_FRAME_DEFAULT &&
       fn->frame_policy != AST_HIR_FRAME_AUTO))
    return 0;
  for (uint32_t i = 0; i < fn->instruction_count; ++i)
    if (fn->instructions[i].info.op == AST_MIR_OP_NATIVE ||
        fn->instructions[i].info.op == AST_MIR_OP_NATIVE_RESULT ||
        fn->instructions[i].info.op == AST_MIR_OP_BIND)
      return 0;
  return 1;
}

/* A stack address may be used for local memory, but cannot escape its original
 * frame through returns, calls, stored pointers or pointer-to-integer tricks.
 */
static int inline_stack_safe(const AstMirFunction *fn) {
  for (uint32_t s = 0; s < fn->stack_slot_count; ++s)
    if (fn->stack_slots[s].info.flags & AST_MIR_STACK_SLOT_DYNAMIC_SIZE)
      return 0;
  uint8_t *local = (uint8_t *)calloc(fn->vreg_count ? fn->vreg_count : 1u, 1u);
  if (!local)
    return -1;
  for (uint32_t i = 0; i < fn->instruction_count; ++i)
    if (fn->instructions[i].info.op == AST_MIR_OP_STACK_ADDR)
      local[fn->instructions[i].info.result] = 1u;
  int changed = 1, safe = 1;
  while (changed && safe) {
    changed = 0;
    for (uint32_t b = 0; b < fn->block_count && safe; ++b) {
      const AstMirBlock *block = &fn->blocks[b];
      for (uint32_t j = 0; j < block->instruction_count && safe; ++j) {
        const AstMirInstruction *inst =
            &fn->instructions[block->instructions[j]];
        for (uint32_t a = 0; a < inst->info.operand_count; ++a) {
          if (!local[inst->operands[a]])
            continue;
          AstMirOp op = inst->info.op;
          if ((op == AST_MIR_OP_LOAD || op == AST_MIR_OP_STORE) && a == 0u)
            continue;
          if (op != AST_MIR_OP_COPY && op != AST_MIR_OP_INDEX &&
              op != AST_MIR_OP_MEMBER && op != AST_MIR_OP_ADDRESS_OF &&
              op != AST_MIR_OP_CAST) {
            safe = 0;
            break;
          }
          if (op == AST_MIR_OP_CAST &&
              ast_sema_type_kind(inst->info.type) != AST_SEMA_TYPE_PTR) {
            safe = 0;
            break;
          }
          uint32_t v = inst->info.result;
          if (v != AST_MIR_INVALID_ID && !local[v]) {
            local[v] = 1u;
            changed = 1;
          }
        }
      }
      const AstMirTerminatorInfo *term = &block->term.info;
      if (term->kind == AST_MIR_TERM_RETURN &&
          term->condition_or_value != AST_MIR_INVALID_ID &&
          local[term->condition_or_value]) {
        safe = 0;
        break;
      }
      unsigned edges = term->kind == AST_MIR_TERM_COND_BR ? 2u
                       : term->kind == AST_MIR_TERM_BR    ? 1u
                                                          : 0u;
      for (unsigned e = 0; e < edges; ++e) {
        const AstMirVReg *args = e ? term->false_args : term->true_args;
        uint32_t count = e ? term->false_arg_count : term->true_arg_count;
        const AstMirBlock *target =
            &fn->blocks[e ? term->target_false : term->target_true];
        for (uint32_t a = 0; a < count; ++a)
          if (local[args[a]] && !local[target->arguments[a]]) {
            local[target->arguments[a]] = 1u;
            changed = 1;
          }
      }
    }
  }
  free(local);
  return safe;
}

static uint32_t inline_site_depth(const AstMirFunction *fn, uint32_t site) {
  uint32_t depth = 0;
  while (site && depth <= fn->inline_site_count) {
    depth++;
    site = fn->inline_sites[site - 1u].parent;
  }
  return depth;
}

static uint32_t inline_max_depth(const AstMirFunction *fn) {
  uint32_t depth = 0;
  for (uint32_t s = 1; s <= fn->inline_site_count; ++s) {
    uint32_t next = inline_site_depth(fn, s);
    if (next > depth)
      depth = next;
  }
  return depth;
}

typedef struct InlineEdit {
  AstMirFunction *output;
  const AstMirFunction *caller, *callee;
  uint32_t *caller_blocks, *callee_blocks, *callee_regs;
  uint32_t block, position, continuation, slot_base, site;
} InlineEdit;

static int inline_emit(InlineEdit *edit, uint32_t block,
                       const AstMirInstruction *old, int callee) {
  AstMirInstruction inst = *old;
  uint32_t n = inst.info.operand_count;
  uint32_t *args = n ? (uint32_t *)mir_malloc_array(n, sizeof(*args)) : NULL;
  if (n && !args)
    return 0;
  for (uint32_t a = 0; a < n; ++a)
    args[a] = callee ? edit->callee_regs[old->operands[a]] : old->operands[a];
  if (callee) {
    inst.info.result = old->info.result == AST_MIR_INVALID_ID
                           ? AST_MIR_INVALID_ID
                           : edit->callee_regs[old->info.result];
    inst.info.source_local = AST_HIR_INVALID_ID;
    inst.info.source_hir_instruction = AST_HIR_INVALID_ID;
    inst.info.inline_site =
        old->info.inline_site ? edit->site + old->info.inline_site : edit->site;
    if (inst.info.op == AST_MIR_OP_STACK_ADDR ||
        inst.info.op == AST_MIR_OP_STACK_LOAD ||
        inst.info.op == AST_MIR_OP_STACK_STORE)
      inst.info.stack_slot += edit->slot_base;
  }
  uint32_t id = mir_emit_instruction(edit->output, block, inst, args, n);
  free(args);
  if (id == AST_MIR_INVALID_ID)
    return 0;
  if (inst.info.result != AST_MIR_INVALID_ID)
    edit->output->vregs[inst.info.result].info.definition_index = id;
  return 1;
}

static int inline_term(InlineEdit *edit, uint32_t block,
                       const AstMirTerminator *old, int callee) {
  AstMirTerminator term = {0};
  term.info = old->info;
  const uint32_t *blocks = callee ? edit->callee_blocks : edit->caller_blocks;
  if (callee && term.info.condition_or_value != AST_MIR_INVALID_ID)
    term.info.condition_or_value =
        edit->callee_regs[term.info.condition_or_value];
  if (callee && term.info.kind == AST_MIR_TERM_RETURN) {
    uint32_t value = term.info.condition_or_value;
    term.info.kind = AST_MIR_TERM_BR;
    term.info.condition_or_value = AST_MIR_INVALID_ID;
    term.info.target_true = edit->continuation;
    term.info.target_false = AST_MIR_INVALID_ID;
    const AstMirInstructionInfo *call =
        &edit->caller
             ->instructions[edit->caller->blocks[edit->block]
                                .instructions[edit->position]]
             .info;
    /* The frozen call result can include an allowed contextual conversion
       (for example ptr<u8> -> ptr<const u8>). Preserve it on inlined returns
       instead of constructing a phi with differently typed incoming values. */
    if (call->result != AST_MIR_INVALID_ID) {
      AstMirFunction *fn = edit->output;
      const AstSemaType *type = edit->caller->vregs[call->result].info.type;
      if (value == AST_MIR_INVALID_ID || value >= fn->vreg_count)
        return 0;
      if (!mir_type_equal(type, fn->vregs[value].info.type)) {
        AstMirVReg converted = mir_add_vreg(fn, AST_MIR_VREG_INSTRUCTION_RESULT,
                                            type, call->span, block, 0u, 0u);
        if (converted == AST_MIR_INVALID_ID)
          return 0;
        AstMirInstruction cast = {0};
        cast.info.op = AST_MIR_OP_CAST;
        cast.info.type = type;
        cast.info.result = converted;
        cast.info.span = call->span;
        cast.info.token_op = AST_TOK_KW_CAST;
        cast.info.flags = AST_MIR_INST_SYNTHETIC;
        cast.info.source_local = AST_HIR_INVALID_ID;
        cast.info.source_hir_instruction = AST_HIR_INVALID_ID;
        cast.info.inline_site = edit->site;
        cast.info.memory_in = cast.info.memory_out = AST_MIR_INVALID_ID;
        uint32_t id = mir_emit_instruction(fn, block, cast, &value, 1u);
        if (id == AST_MIR_INVALID_ID)
          return 0;
        fn->vregs[converted].info.definition_index = id;
        value = converted;
      }
    }
    return mir_set_terminator(
        edit->output, block, term,
        call->result == AST_MIR_INVALID_ID ? NULL : &value,
        call->result == AST_MIR_INVALID_ID ? 0u : 1u, NULL, 0u);
  }
  uint32_t *args[2] = {NULL, NULL};
  uint32_t counts[2] = {0, 0};
  unsigned edges = term.info.kind == AST_MIR_TERM_COND_BR ? 2u
                   : term.info.kind == AST_MIR_TERM_BR    ? 1u
                                                          : 0u;
  int ok = 1;
  for (unsigned e = 0; e < edges; ++e) {
    uint32_t target = e ? old->info.target_false : old->info.target_true;
    if (e)
      term.info.target_false = blocks[target];
    else
      term.info.target_true = blocks[target];
    const uint32_t *values = e ? old->false_args : old->true_args;
    counts[e] = e ? old->info.false_arg_count : old->info.true_arg_count;
    args[e] = counts[e]
                  ? (uint32_t *)mir_malloc_array(counts[e], sizeof(**args))
                  : NULL;
    if (counts[e] && !args[e]) {
      ok = 0;
      break;
    }
    for (uint32_t a = 0; a < counts[e]; ++a)
      args[e][a] = callee ? edit->callee_regs[values[a]] : values[a];
  }
  if (ok)
    ok = mir_set_terminator(edit->output, block, term, args[0], counts[0],
                            args[1], counts[1]);
  free(args[0]);
  free(args[1]);
  return ok;
}

static AstMirFunction *inline_splice(const AstMirFunction *caller,
                                     const AstMirFunction *callee,
                                     uint32_t call_block,
                                     uint32_t call_position) {
  InlineEdit edit = {0};
  edit.caller = caller;
  edit.callee = callee;
  edit.block = call_block;
  edit.position = call_position;
  AstMirFunction *fn = edit.output = mir_clone_function_header(caller);
  if (!fn)
    return NULL;
  const AstMirInstructionInfo *call =
      &caller
           ->instructions[caller->blocks[call_block]
                              .instructions[call_position]]
           .info;
  uint64_t blocks = (uint64_t)caller->block_count + callee->block_count + 1u;
  uint64_t sites =
      (uint64_t)caller->inline_site_count + callee->inline_site_count + 1u;
  uint64_t slots =
      (uint64_t)caller->stack_slot_count + callee->stack_slot_count;
  if (blocks > UINT32_MAX || sites > UINT32_MAX || slots > UINT32_MAX ||
      (uint64_t)caller->vreg_count + callee->vreg_count > UINT32_MAX)
    goto fail;
  edit.caller_blocks =
      (uint32_t *)mir_malloc_array(caller->block_count, sizeof(uint32_t));
  edit.callee_blocks =
      (uint32_t *)mir_malloc_array(callee->block_count, sizeof(uint32_t));
  edit.callee_regs = (uint32_t *)mir_malloc_array(
      callee->vreg_count ? callee->vreg_count : 1u, sizeof(uint32_t));
  fn->blocks = (AstMirBlock *)calloc((size_t)blocks, sizeof(*fn->blocks));
  fn->vregs = (AstMirVRegData *)mir_malloc_array(
      caller->vreg_count ? caller->vreg_count : 1u, sizeof(*fn->vregs));
  if (!edit.caller_blocks || !edit.callee_blocks || !edit.callee_regs ||
      !fn->blocks || !fn->vregs)
    goto fail;
  fn->block_count = (uint32_t)blocks;
  fn->vreg_count = fn->vreg_capacity = caller->vreg_count;
  if (caller->vreg_count)
    memcpy(fn->vregs, caller->vregs,
           (size_t)caller->vreg_count * sizeof(*fn->vregs));
  for (uint32_t b = 0; b < caller->block_count; ++b)
    edit.caller_blocks[b] = b <= call_block ? b : b + callee->block_count + 1u;
  for (uint32_t b = 0; b < callee->block_count; ++b)
    edit.callee_blocks[b] = call_block + 1u + b;
  edit.continuation = call_block + callee->block_count + 1u;
  fn->entry_block = edit.caller_blocks[caller->entry_block];
  for (uint32_t b = 0; b < fn->block_count; ++b)
    fn->blocks[b].id = b;
  for (uint32_t v = 0; v < caller->vreg_count; ++v)
    fn->vregs[v].info.block = edit.caller_blocks[caller->vregs[v].info.block];
  edit.slot_base = caller->stack_slot_count;
  if (callee->stack_slot_count) {
    AstMirStackSlot *next = (AstMirStackSlot *)realloc(
        fn->stack_slots, (size_t)slots * sizeof(*next));
    if (!next)
      goto fail;
    fn->stack_slots = next;
    fn->stack_slot_count = fn->stack_slot_capacity = (uint32_t)slots;
    for (uint32_t s = 0; s < callee->stack_slot_count; ++s) {
      fn->stack_slots[edit.slot_base + s] = callee->stack_slots[s];
      AstMirStackSlotInfo *info = &fn->stack_slots[edit.slot_base + s].info;
      info->id = edit.slot_base + s;
      info->source_local = AST_HIR_INVALID_ID;
      info->flags &= ~(uint32_t)AST_MIR_STACK_SLOT_PARAMETER;
    }
  }
  AstMirInlineSiteInfo *site_table = (AstMirInlineSiteInfo *)realloc(
      fn->inline_sites, (size_t)sites * sizeof(*site_table));
  if (!site_table)
    goto fail;
  fn->inline_sites = site_table;
  edit.site = caller->inline_site_count + 1u;
  fn->inline_site_count = (uint32_t)sites;
  fn->inline_sites[edit.site - 1u] = (AstMirInlineSiteInfo){
      callee->module_name, callee->name, call->span, call->inline_site};
  for (uint32_t s = 0; s < callee->inline_site_count; ++s) {
    fn->inline_sites[edit.site + s] = callee->inline_sites[s];
    fn->inline_sites[edit.site + s].parent =
        callee->inline_sites[s].parent
            ? edit.site + callee->inline_sites[s].parent
            : edit.site;
  }
  for (uint32_t v = 0; v < callee->vreg_count; ++v) {
    const AstMirVRegInfo *info = &callee->vregs[v].info;
    if (info->kind == AST_MIR_VREG_BLOCK_ARGUMENT &&
        info->block == callee->entry_block) {
      edit.callee_regs[v] = call->operands[info->definition_index];
      continue;
    }
    edit.callee_regs[v] =
        mir_add_vreg(fn, info->kind, info->type, info->span,
                     edit.callee_blocks[info->block], info->definition_index,
                     info->flags & ~(uint32_t)AST_MIR_VREG_FUNCTION_ARGUMENT);
    if (edit.callee_regs[v] == AST_MIR_INVALID_ID)
      goto fail;
  }
  if (call->result != AST_MIR_INVALID_ID) {
    AstMirVRegInfo *result = &fn->vregs[call->result].info;
    result->kind = AST_MIR_VREG_BLOCK_ARGUMENT;
    result->block = edit.continuation;
    result->definition_index = 0;
    result->flags = AST_MIR_VREG_PHI;
    if (!mir_append_block_argument(fn, edit.continuation, call->result,
                                   AST_HIR_INVALID_ID))
      goto fail;
  }
  for (uint32_t b = 0; b < caller->block_count; ++b) {
    const AstMirBlock *old = &caller->blocks[b];
    uint32_t to = edit.caller_blocks[b];
    fn->blocks[to].span = old->span;
    fn->blocks[to].hir_argument_count = old->hir_argument_count;
    for (uint32_t a = 0; a < old->argument_count; ++a)
      if (!mir_append_block_argument(fn, to, old->arguments[a],
                                     old->argument_source_locals[a]))
        goto fail;
    for (uint32_t j = 0; j < old->instruction_count; ++j) {
      if (b == call_block && j == call_position) {
        to = edit.continuation;
        continue;
      }
      const AstMirInstruction *inst =
          &caller->instructions[old->instructions[j]];
      if (inst->info.result != AST_MIR_INVALID_ID)
        fn->vregs[inst->info.result].info.block = to;
      if (!inline_emit(&edit, to, inst, 0))
        goto fail;
    }
    if (!inline_term(&edit, to, &old->term, 0))
      goto fail;
  }
  AstMirTerminator enter = {0};
  enter.info.kind = AST_MIR_TERM_BR;
  enter.info.span = call->span;
  enter.info.condition_or_value = AST_MIR_INVALID_ID;
  enter.info.target_true = edit.callee_blocks[callee->entry_block];
  enter.info.target_false = AST_MIR_INVALID_ID;
  if (!mir_set_terminator(fn, call_block, enter, NULL, 0u, NULL, 0u))
    goto fail;
  fn->blocks[edit.continuation].span = call->span;
  for (uint32_t b = 0; b < callee->block_count; ++b) {
    const AstMirBlock *old = &callee->blocks[b];
    uint32_t to = edit.callee_blocks[b];
    fn->blocks[to].span = old->span;
    if (b != callee->entry_block)
      for (uint32_t a = 0; a < old->argument_count; ++a)
        if (!mir_append_block_argument(fn, to,
                                       edit.callee_regs[old->arguments[a]],
                                       AST_HIR_INVALID_ID))
          goto fail;
    for (uint32_t j = 0; j < old->instruction_count; ++j)
      if (!inline_emit(&edit, to, &callee->instructions[old->instructions[j]],
                       1))
        goto fail;
    if (!inline_term(&edit, to, &old->term, 1))
      goto fail;
  }
  if (!inline_finish(fn))
    goto fail;
  free(edit.caller_blocks);
  free(edit.callee_blocks);
  free(edit.callee_regs);
  return fn;
fail:
  free(edit.caller_blocks);
  free(edit.callee_blocks);
  free(edit.callee_regs);
  mir_function_destroy(fn);
  return NULL;
}

AstMirInlineConfig ast_mir_inline_config_default(void) {
  AstMirInlineConfig config = {
      AST_MIR_INLINE_BALANCED, 256u, 8u, 128u, 4096u, 50u, 0u};
  return config;
}

static void inline_destroy_functions(AstMirFunction **functions, size_t count) {
  if (functions)
    for (size_t f = 0; f < count; ++f)
      mir_function_destroy(functions[f]);
  free(functions);
}

void mir_inline_proof_destroy(MirInlineProof *proof) {
  if (!proof)
    return;
  inline_destroy_functions(proof->original, proof->function_count);
  free(proof);
}

static uint32_t inline_call_count(const InlineGraph *g,
                                  AstMirFunction *const *functions,
                                  uint32_t callee) {
  uint32_t count = 0;
  for (uint32_t f = 0; f < g->count; ++f)
    for (uint32_t i = 0; i < functions[f]->instruction_count; ++i) {
      const AstMirInstructionInfo *inst = &functions[f]->instructions[i].info;
      if (inst->op == AST_MIR_OP_CALL_DIRECT &&
          inline_resolve(g, functions[f], inst) == callee &&
          count != UINT32_MAX)
        count++;
    }
  return count;
}

static AstMirFunction **inline_build(AstMirProgram *program,
                                     const AstMirInlineConfig *config,
                                     AstMirInlineStats *stats) {
  InlineGraph graph = {0};
  AstMirFunction **functions = NULL;
  if (!inline_graph_build(&graph, program->functions, program->function_count))
    goto fail;
  functions = (AstMirFunction **)calloc(
      program->function_count ? program->function_count : 1u,
      sizeof(*functions));
  if (!functions)
    goto fail;
  for (size_t f = 0; f < program->function_count; ++f) {
    functions[f] = inline_clone(program->functions[f]);
    if (!functions[f])
      goto fail;
    stats->instructions_before += functions[f]->instruction_count;
  }
  uint64_t total = stats->instructions_before;
  uint64_t growth = total / 100u * config->max_module_growth_percent +
                    total % 100u * config->max_module_growth_percent / 100u;
  uint64_t budget =
      total > UINT64_MAX - growth - 32u ? UINT64_MAX : total + growth + 32u;
  /* Forced requests get the resource budget before ordinary candidates. */
  for (unsigned priority = 0; priority < 2u; ++priority)
    for (uint32_t order = 0; order < graph.count; ++order) {
      uint32_t f = graph.order[order], i = 0;
      if (!inline_frame_safe(functions[f]))
        continue;
      while (i < functions[f]->instruction_count &&
             stats->inlined < config->max_sites) {
        AstMirFunction *caller = functions[f];
        const AstMirInstructionInfo *call = &caller->instructions[i].info;
        if (call->op != AST_MIR_OP_CALL_DIRECT) {
          i++;
          continue;
        }
        uint32_t target = inline_resolve(&graph, caller, call);
        if (target == AST_MIR_INVALID_ID) {
          if (priority) {
            stats->considered++;
            stats->rejected_contract++;
          }
          i++;
          continue;
        }
        const AstMirFunction *callee = functions[target];
        if (!!(callee->flags & AST_HIR_FUNCTION_INLINE) != !priority) {
          i++;
          continue;
        }
        stats->considered++;
        if (graph.recursive[target]) {
          stats->rejected_recursive++;
          i++;
          continue;
        }
        if (!inline_frame_safe(callee) ||
            (callee->flags & AST_HIR_FUNCTION_NO_INLINE) ||
            (!config->closed_world &&
             (callee->flags & AST_HIR_FUNCTION_EXPORT)) ||
            callee->link_name.is_present ||
            callee->target_constraint.is_present ||
            callee->requires_constraint.is_present ||
            callee->stack_alignment > 16u ||
            !mir_type_equal(call->call_signature, callee->type) ||
            callee->blocks[callee->entry_block].predecessor_count ||
            call->operand_count !=
                callee->blocks[callee->entry_block].argument_count) {
          stats->rejected_contract++;
          i++;
          continue;
        }
        uint32_t depth = inline_site_depth(caller, call->inline_site) + 1u +
                         inline_max_depth(callee);
        if (depth > config->max_depth ||
            callee->instruction_count > config->max_callee_instructions ||
            (uint64_t)caller->inline_site_count + callee->inline_site_count +
                    1u >
                config->max_sites ||
            callee->vreg_count > config->max_function_instructions * 8u ||
            (uint64_t)caller->instruction_count + callee->instruction_count >
                config->max_function_instructions) {
          stats->rejected_budget++;
          i++;
          continue;
        }
        int stack_safe = inline_stack_safe(callee);
        if (stack_safe < 0)
          goto fail;
        if (!stack_safe) {
          stats->rejected_contract++;
          i++;
          continue;
        }
        uint32_t block = 0, position = 0;
        int found = 0;
        for (uint32_t b = 0; b < caller->block_count && !found; ++b)
          for (uint32_t j = 0; j < caller->blocks[b].instruction_count; ++j)
            if (caller->blocks[b].instructions[j] == i) {
              block = b;
              position = j;
              found = 1;
              break;
            }
        if (!found) {
          mir_program_diag(program, AST_MIR_DIAG_INTERNAL, AST_DIAG_ERROR,
                           call->span,
                           "inline call in '%.*s' has no owning block",
                           (int)caller->name.span.length, caller->name.text);
          goto fail;
        }
        AstMirFunction *spliced =
            inline_splice(caller, callee, block, position);
        if (!spliced) {
          mir_program_diag(program, AST_MIR_DIAG_INTERNAL, AST_DIAG_ERROR,
                           call->span,
                           "cannot splice inline call '%.*s' -> '%.*s'",
                           (int)caller->name.span.length, caller->name.text,
                           (int)callee->name.span.length, callee->name.text);
          goto fail;
        }
        AstMirOptimizeStats cleanup = {0};
        AstMirFunction *candidate =
            mir_optimize_function(program, spliced, &cleanup);
        mir_function_destroy(spliced);
        if (!candidate) {
          mir_program_diag(program, AST_MIR_DIAG_INTERNAL, AST_DIAG_ERROR,
                           call->span,
                           "cannot normalize inline call '%.*s' -> '%.*s'",
                           (int)caller->name.span.length, caller->name.text,
                           (int)callee->name.span.length, callee->name.text);
          goto fail;
        }
        uint64_t after =
            total - caller->instruction_count + candidate->instruction_count;
        uint64_t credit = inline_call_count(&graph, functions, target) == 1u
                              ? callee->instruction_count
                              : 3u;
        if (after > budget ||
            (config->mode == AST_MIR_INLINE_SIZE &&
             !(callee->flags & AST_HIR_FUNCTION_INLINE) &&
             candidate->instruction_count >
                 (uint64_t)caller->instruction_count + credit)) {
          mir_function_destroy(candidate);
          stats->rejected_cost++;
          i++;
          continue;
        }
        functions[f] = candidate;
        mir_function_destroy(caller);
        total = after;
        stats->inlined++;
        if (depth > stats->max_depth)
          stats->max_depth = depth;
        if (callee->flags & AST_HIR_FUNCTION_INLINE)
          stats->forced++;
        i = 0;
      }
    }
  /* An enabled pass never silently drops an explicit request. Safety, SCC
   * and hard resource budgets remain mandatory even for forced sites. */
  for (uint32_t f = 0; f < graph.count; ++f)
    for (uint32_t i = 0; i < functions[f]->instruction_count; ++i) {
      const AstMirInstructionInfo *call = &functions[f]->instructions[i].info;
      if (call->op != AST_MIR_OP_CALL_DIRECT)
        continue;
      uint32_t target = inline_resolve(&graph, functions[f], call);
      if (target != AST_MIR_INVALID_ID &&
          (functions[target]->flags & AST_HIR_FUNCTION_INLINE)) {
        mir_program_diag(program, AST_MIR_DIAG_INLINE_POLICY, AST_DIAG_ERROR,
                         call->span,
                         "cannot satisfy @inline for '%.*s': safety, linkage, "
                         "recursion or hard resource budget",
                         (int)functions[target]->name.span.length,
                         functions[target]->name.text);
        goto fail;
      }
    }
  stats->instructions_after = total;
  inline_graph_destroy(&graph);
  return functions;
fail:
  inline_graph_destroy(&graph);
  inline_destroy_functions(functions, program->function_count);
  return NULL;
}

static void inline_view_dispose(AstMirProgram *view) {
  for (size_t d = 0; d < view->diagnostic_count; ++d)
    free((void *)view->diagnostics[d].message);
  free(view->diagnostics);
}

static int inline_config_valid(const AstMirInlineConfig *config) {
  return (unsigned)config->mode <= AST_MIR_INLINE_SIZE &&
         config->max_sites <= 4096u && config->max_depth <= 64u &&
         config->max_callee_instructions <= 65536u &&
         config->max_function_instructions <= 1048576u &&
         config->max_module_growth_percent <= 400u &&
         config->closed_world <= 1u;
}

int mir_verify_inlining(AstMirProgram *program) {
  MirInlineProof *proof = program->inline_proof;
  if (!proof || !proof->original ||
      proof->function_count != program->function_count ||
      !inline_config_valid(&proof->config) ||
      proof->config.mode == AST_MIR_INLINE_NONE)
    return 0;
  AstMirProgram view = *program;
  view.functions = proof->original;
  view.inline_proof = NULL;
  view.diagnostics = NULL;
  view.diagnostic_count = view.diagnostic_capacity =
      view.build_diagnostic_count = 0;
  int ok = ast_mir_verify_mode(&view, AST_MIR_VERIFY_POST_OPT) &&
           ast_mir_verify_structural(program);
  AstMirInlineStats ignored = {0};
  AstMirFunction **expected = NULL;
  if (ok) {
    expected = inline_build(&view, &proof->config, &ignored);
    ok = expected != NULL;
  }
  for (size_t f = 0; ok && f < program->function_count; ++f)
    ok = !program->functions[f]->optimization_proof &&
         !program->functions[f]->gvn_certificates &&
         !program->functions[f]->branch_certificates &&
         mir_functions_equal(program->functions[f], expected[f]);
  inline_destroy_functions(expected, program->function_count);
  inline_view_dispose(&view);
  if (!ok)
    mir_program_diag(program, AST_MIR_DIAG_HIR_INVALID, AST_DIAG_ERROR,
                     (AstSpan){0},
                     "inlined MIR differs from verified whole-module replay");
  return ok;
}

int ast_mir_inline(AstMirProgram *program, const AstMirInlineConfig *selected,
                   AstMirInlineStats *stats) {
  AstMirInlineConfig config =
      selected ? *selected : ast_mir_inline_config_default();
  if (!program || program->inline_proof || !inline_config_valid(&config) ||
      !ast_mir_verify_mode(program, AST_MIR_VERIFY_POST_OPT))
    return 0;
  AstMirInlineStats counts = {0};
  if (config.mode == AST_MIR_INLINE_NONE || !config.max_sites ||
      !config.max_depth) {
    if (stats)
      *stats = counts;
    return 1;
  }
  AstMirFunction **candidate = inline_build(program, &config, &counts);
  if (!candidate) {
    if (!program->diagnostic_count)
      mir_program_diag(
          program, AST_MIR_DIAG_INTERNAL, AST_DIAG_ERROR, (AstSpan){0},
          "cannot construct inlined MIR; original module retained");
    return 0;
  }
  if (!counts.inlined) {
    inline_destroy_functions(candidate, program->function_count);
    if (stats)
      *stats = counts;
    return 1;
  }
  MirInlineProof *proof = (MirInlineProof *)calloc(1u, sizeof(*proof));
  if (!proof) {
    inline_destroy_functions(candidate, program->function_count);
    return 0;
  }
  proof->original = program->functions;
  proof->function_count = program->function_count;
  proof->config = config;
  program->functions = candidate;
  program->inline_proof = proof;
  if (!ast_mir_verify_mode(program, AST_MIR_VERIFY_POST_OPT)) {
    program->functions = proof->original;
    program->inline_proof = NULL;
    proof->original = NULL;
    free(proof);
    inline_destroy_functions(candidate, program->function_count);
    (void)ast_mir_verify(program);
    return 0;
  }
  if (stats)
    *stats = counts;
  return 1;
}
