#include "allocation_internal.h"

AstRegAllocConfig ast_regalloc_config_default(void) {
  AstRegAllocConfig c = {0};
  c.verify_machine = 1u;
  c.verify_allocation = 1u;
  c.enable_coalescing = 1u;
  c.enable_rematerialization = 1u;
  c.enable_red_zone = 1u;
  return c;
}
int regalloc_fail(AstRegAllocProgram *p, AstRegAllocDiagCode code, uint32_t fn,
                  uint32_t inst, const char *message) {
  if (!p)
    return 0;
  if (!p->stats.error_count) {
    p->diagnostic.code = code;
    p->diagnostic.function_index = fn;
    p->diagnostic.instruction = inst;
    p->diagnostic.message = message;
    if (fn < p->function_count &&
        inst < p->machine->functions[fn].info.instruction_count)
      p->diagnostic.span =
          p->machine->functions[fn].instructions[inst].info.span;
    p->stats.error_count = 1u;
  }
  return 0;
}
uint32_t regalloc_location_piece_count(const AllocationFunction *fn,
                                       uint32_t vreg) {
  return fn && fn->location_piece_counts && vreg < fn->liveness.vreg_count
             ? fn->location_piece_counts[vreg]
             : 0u;
}
const AstRegAllocLocation *regalloc_location_piece(const AllocationFunction *fn,
                                                   uint32_t vreg,
                                                   uint32_t piece) {
  if (!fn || !fn->location_pieces || vreg >= fn->liveness.vreg_count ||
      !fn->location_pieces[vreg] ||
      piece >= regalloc_location_piece_count(fn, vreg))
    return NULL;
  return &fn->location_pieces[vreg][piece].location;
}
const AstRegAllocLocation *regalloc_location_at(const AllocationFunction *fn,
                                                uint32_t vreg, uint32_t slot) {
  if (!fn || !fn->locations || vreg >= fn->liveness.vreg_count)
    return NULL;
  uint32_t count = regalloc_location_piece_count(fn, vreg);
  if (!count || !fn->location_pieces[vreg])
    return &fn->locations[vreg];
  const RegAllocLocationPiece *pieces = fn->location_pieces[vreg];
  uint32_t lo = 0u, hi = count;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2u;
    if (pieces[mid].end <= slot)
      lo = mid + 1u;
    else
      hi = mid;
  }
  if (lo < count && pieces[lo].begin <= slot)
    return &pieces[lo].location;
  /* Definitions start one slot after their instruction. */
  if (slot != UINT32_MAX) {
    uint32_t next = slot + 1u;
    lo = 0u;
    hi = count;
    while (lo < hi) {
      uint32_t mid = lo + (hi - lo) / 2u;
      if (pieces[mid].end <= next)
        lo = mid + 1u;
      else
        hi = mid;
    }
    if (lo < count && pieces[lo].begin <= next)
      return &pieces[lo].location;
  }
  return &fn->locations[vreg];
}

int regalloc_unit_live_at(const AllocationFunction *fn, uint32_t unit,
                          uint32_t slot) {
  if (!fn || unit >= MACHINE_REGISTER_UNITS || !fn->unit_range_offsets ||
      !fn->unit_range_storage || slot == UINT32_MAX)
    return 1;
  uint32_t lo = fn->unit_range_offsets[unit],
           hi = fn->unit_range_offsets[unit + 1u];
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2u;
    if (fn->unit_range_storage[mid].end <= slot)
      lo = mid + 1u;
    else
      hi = mid;
  }
  return lo < fn->unit_range_offsets[unit + 1u] &&
         fn->unit_range_storage[lo].begin <= slot;
}
static int allocator_reserved(const AstTargetConfig *tc,
                              const MachinePhysRegView *v) {
  if (v->reserved || v->special)
    return 1;
  if (v->file == MACHINE_PHYSREG_FILE_GPR) {
    if (tc->arch == AST_TARGET_ARCH_X86_64)
      return v->index == 5u; /* frame pointer */
    if (tc->arch == AST_TARGET_ARCH_AARCH64)
      return v->index == 18u || v->index >= 29u;
    if (tc->arch == AST_TARGET_ARCH_RISCV64)
      return v->index == 1u || v->index == 8u;
  }
  return tc->arch == AST_TARGET_ARCH_X86_64 &&
         v->file == MACHINE_PHYSREG_FILE_MASK_PREDICATE && v->index == 0u;
}
typedef struct RegisterCandidate {
  uint32_t unit, preserved;
  char name[AST_MACHINE_REGISTER_NAME_CAP];
} RegisterCandidate;
typedef struct RegAllocRegisterSet {
  struct RegAllocRegisterSet *next;
  AstMachineRegBank bank;
  uint32_t bits, count;
  RegisterCandidate candidates[32];
} RegAllocRegisterSet;
/* Target/ABI are immutable within an allocation program. Enumerate each bank
   and width once, retaining the architectural order used for tie breaking. */
static const RegAllocRegisterSet *
register_set(AstRegAllocProgram *p, AstMachineRegBank bank, uint32_t bits) {
  for (RegAllocRegisterSet *s = p->register_sets; s; s = s->next)
    if (s->bank == bank && s->bits == bits)
      return s;
  RegAllocRegisterSet *s = (RegAllocRegisterSet *)calloc(1u, sizeof(*s));
  if (!s)
    return NULL;
  s->bank = bank;
  s->bits = bits;
  for (uint32_t r = 0; r < 32u; ++r) {
    RegisterCandidate *candidate = &s->candidates[s->count];
    MachinePhysRegView view;
    if (!machine_physreg_name(&p->target, bank, r, bits, candidate->name) ||
        !machine_physreg_view(&p->target, candidate->name, &view) ||
        allocator_reserved(&p->target, &view))
      continue;
    candidate->unit = view.alias_unit;
    candidate->preserved = machine_physreg_preserved_bits(p->abi, &view);
    ++s->count;
  }
  s->next = p->register_sets;
  p->register_sets = s;
  return s;
}
/* ABI argument roles enumerate registers in the value's register bank, rather
   than inspecting this function's signature (arg0 is legal in a leaf with no
   source-language parameters). Explicit ISA names are checked before stripping.
 */
static int bind_view(const AstRegAllocProgram *p, uint32_t f,
                     const MachineInstruction *mi, uint32_t *value,
                     MachinePhysRegView *out) {
  const MachineFunction *mf = &p->machine->functions[f];
  AstMirInstructionInfo source = {0};
  if (mi->info.source_op != AST_MIR_OP_BIND || mi->info.operand_count != 1u ||
      mi->info.operands[0].kind != AST_MACHINE_OPERAND_VREG ||
      !ast_mir_function_instruction(ast_mir_function_at(p->machine->mir, f),
                                    mi->info.source_instruction, &source))
    return 0;
  uint32_t v = mi->info.operands[0].as.vreg;
  if (v >= mf->info.vreg_count || !source.symbol.text ||
      !source.symbol.span.length || source.symbol.span.length >= 64u)
    return 0;
  AstMirFunctionInfo fi = {0};
  if (!ast_mir_function_info(ast_mir_function_at(p->machine->mir, f), &fi))
    return 0;
  for (uint32_t s = 0; s < fi.stack_slot_count; ++s) {
    AstMirStackSlotInfo slot;
    if (!ast_mir_function_stack_slot(ast_mir_function_at(p->machine->mir, f), s,
                                     &slot))
      return 0;
    if (slot.source_local == source.source_local)
      return 0; /* address-taken/non-SSA bind */
  }
  char text[64] = {0}, name[AST_MACHINE_REGISTER_NAME_CAP] = {0};
  for (uint32_t i = 0; i < source.symbol.span.length; ++i) {
    unsigned char ch = (unsigned char)source.symbol.text[i];
    text[i] = (char)(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
  }
  char *reg = text;
  if (reg[0] == '"' && source.symbol.span.length > 2u &&
      text[source.symbol.span.length - 1u] == '"') {
    text[source.symbol.span.length - 1u] = 0;
    ++reg;
  }
  char *dot = strchr(reg, '.');
  if (dot) {
    *dot = 0;
    reg = dot + 1;
    int match = p->target.arch == AST_TARGET_ARCH_X86_64
                    ? (!strcmp(text, "x86") || !strcmp(text, "x86_64"))
                : p->target.arch == AST_TARGET_ARCH_AARCH64
                    ? (!strcmp(text, "aarch64") || !strcmp(text, "a64"))
                    : (!strcmp(text, "riscv64") || !strcmp(text, "rv64"));
    if (!match)
      return 0;
  }
  AstMachineRegBank bank = mf->vregs[v].bank;
  uint32_t bits = mf->vregs[v].bit_width;
  if (!dot && !strncmp(reg, "arg", 3u) && reg[3] >= '0' && reg[3] <= '7' &&
      !reg[4]) {
    uint32_t index = (uint32_t)(reg[3] - '0');
    if (bank != AST_MACHINE_BANK_GPR && bank != AST_MACHINE_BANK_FP &&
        bank != AST_MACHINE_BANK_VECTOR)
      return 0;
    if (p->abi == AST_ABI_SYSV_AMD64 && bank == AST_MACHINE_BANK_GPR) {
      static const uint32_t indices[] = {7u, 6u, 2u, 1u, 8u, 9u};
      if (index >= ASTER_ARRAY_COUNT(indices))
        return 0;
      index = indices[index];
    } else if (p->abi == AST_ABI_WINDOWS_X64) {
      static const uint32_t indices[] = {1u, 2u, 8u, 9u};
      if (index >= ASTER_ARRAY_COUNT(indices))
        return 0;
      if (bank == AST_MACHINE_BANK_GPR)
        index = indices[index];
    } else if (p->target.arch == AST_TARGET_ARCH_RISCV64) {
      if (bank != AST_MACHINE_BANK_GPR &&
          (bank != AST_MACHINE_BANK_FP || p->abi == AST_ABI_RISCV_LP64 ||
           (p->abi == AST_ABI_RISCV_LP64F && bits > 32u)))
        return 0;
      index += 10u;
    }
    if (!machine_physreg_name(&p->target, bank, index, bits, name))
      return 0;
    reg = name;
  }
  AstTargetRegisterInfo raw;
  if (!ast_target_parse_register(&p->target, reg, &raw) || raw.high8 ||
      !machine_physreg_view(&p->target, reg, out) ||
      allocator_reserved(&p->target, out) || !bits || out->width < bits ||
      !machine_register_compatible(&p->target, reg, bank))
    return 0;
  *value = v;
  return 1;
}
int regalloc_verify_bind_instruction(const AstRegAllocProgram *p, uint32_t f,
                                     const MachineInstruction *mi) {
  uint32_t v;
  MachinePhysRegView view;
  if (!bind_view(p, f, mi, &v, &view))
    return 0;
  const AstRegAllocLocation *loc = &p->functions[f].locations[v];
  return loc->kind == AST_REGALLOC_LOCATION_REGISTER &&
         loc->register_unit == view.alias_unit;
}
static int segment_overlap(uint32_t ab, uint32_t ae, uint32_t bb, uint32_t be);
static void set_all_piece_locations(AllocationFunction *af, uint32_t vreg,
                                    const AstRegAllocLocation *location);

static int rematerialize_single_use_constant(AstRegAllocProgram *p, uint32_t f,
                                             uint32_t v) {
  AllocationFunction *af = &p->functions[f];
  const MachineFunction *mf = &p->machine->functions[f];
  if (!p->config.enable_rematerialization || v >= mf->info.vreg_count ||
      mf->vregs[v].bank != AST_MACHINE_BANK_GPR ||
      mf->vregs[v].bit_width > 64u ||
      af->binding_units[v] != AST_MACHINE_INVALID_ID ||
      af->liveness.intervals[v].def_count != 1u ||
      af->liveness.intervals[v].use_count != 1u)
    return 0;
  for (uint32_t i = 0; i < mf->info.instruction_count; ++i) {
    const MachineInstruction *mi = &mf->instructions[i];
    if (mi->info.source_op != AST_MIR_OP_CONSTANT ||
        mi->info.result_count != 1u || mi->info.result_first != v ||
        mi->info.operand_count != 1u ||
        mi->info.operands[0].kind != AST_MACHINE_OPERAND_IMMEDIATE)
      continue;
    AstRegAllocLocation *loc = &af->locations[v];
    loc->kind = AST_REGALLOC_LOCATION_REMATERIALIZE;
    loc->immediate_bits = (uint64_t)mi->info.operands[0].as.immediate;
    set_all_piece_locations(af, v, loc);
    ++p->stats.rematerialized_values;
    return 1;
  }
  return 0;
}

static int spill_value(AstRegAllocProgram *p, uint32_t f, uint32_t v) {
  AllocationFunction *af = &p->functions[f];
  const MachineFunction *mf = &p->machine->functions[f];
  AstRegAllocLocation *loc = &af->locations[v];
  /* Only single-def integer constants are exact rematerialization candidates.
     FP payloads and wide/split constants require source-aware expansion. */
  if (p->config.enable_rematerialization &&
      mf->vregs[v].bank == AST_MACHINE_BANK_GPR &&
      mf->vregs[v].bit_width <= 64u &&
      af->liveness.intervals[v].def_count == 1u) {
    for (uint32_t i = 0; i < mf->info.instruction_count; ++i) {
      const MachineInstruction *mi = &mf->instructions[i];
      if (mi->info.source_op == AST_MIR_OP_CONSTANT &&
          mi->info.result_count == 1u && mi->info.result_first == v &&
          mi->info.operand_count == 1u &&
          mi->info.operands[0].kind == AST_MACHINE_OPERAND_IMMEDIATE) {
        loc->kind = AST_REGALLOC_LOCATION_REMATERIALIZE;
        loc->immediate_bits = (uint64_t)mi->info.operands[0].as.immediate;
        p->stats.rematerialized_values++;
        return 1;
      }
    }
  }
  uint64_t size = ((uint64_t)loc->bit_width + 7u) / 8u;
  uint32_t alignment = 1u;
  while (alignment < 16u && alignment < size)
    alignment *= 2u;
  if (!size)
    return regalloc_fail(p, AST_REGALLOC_DIAG_UNSUPPORTED_VALUE, f,
                         AST_MACHINE_INVALID_ID,
                         "Scalable or unsized spill values are unsupported.");
  uint32_t object = AST_MACHINE_INVALID_ID;
  /* Reuse a slot only when it does not overlap any earlier occupant. */
  for (uint32_t s = 0; s < af->object_count; ++s) {
    if (af->objects[s].kind != AST_FRAME_OBJECT_SPILL ||
        af->objects[s].size < size || af->objects[s].alignment < alignment)
      continue;
    int conflict = 0;
    for (uint32_t u = 0; u < mf->info.vreg_count && !conflict; ++u) {
      if (u == v)
        continue;
      for (uint32_t piece = 0; piece < regalloc_location_piece_count(af, u);
           ++piece) {
        const RegAllocLocationPiece *other = &af->location_pieces[u][piece];
        if (other->location.kind == AST_REGALLOC_LOCATION_SPILL &&
            other->location.frame_object == s &&
            segment_overlap(af->liveness.intervals[v].first_slot,
                            af->liveness.intervals[v].last_slot + 1u,
                            other->begin, other->end)) {
          conflict = 1;
          break;
        }
      }
    }
    if (!conflict) {
      object = s;
      break;
    }
  }
  if (object == AST_MACHINE_INVALID_ID)
    object = frame_add_object(af, AST_FRAME_OBJECT_SPILL, size, alignment);
  if (object == AST_MACHINE_INVALID_ID)
    return regalloc_fail(p, AST_REGALLOC_DIAG_OUT_OF_MEMORY, f,
                         AST_MACHINE_INVALID_ID,
                         "Cannot allocate spill frame object.");
  loc->kind = AST_REGALLOC_LOCATION_SPILL;
  loc->frame_object = object;
  p->stats.spilled_values++;
  return 1;
}

typedef struct AllocationSegmentOrder {
  uint32_t vreg;
  uint32_t piece;
  uint32_t begin;
  uint64_t weight;
} AllocationSegmentOrder;
static int segment_order_compare(const void *a, const void *b) {
  const AllocationSegmentOrder *x = (const AllocationSegmentOrder *)a;
  const AllocationSegmentOrder *y = (const AllocationSegmentOrder *)b;
  if (x->begin != y->begin)
    return x->begin < y->begin ? -1 : 1;
  if (x->weight != y->weight)
    return x->weight > y->weight ? -1 : 1;
  if (x->vreg != y->vreg)
    return x->vreg < y->vreg ? -1 : 1;
  return x->piece < y->piece ? -1 : x->piece > y->piece;
}
static int segment_overlap(uint32_t ab, uint32_t ae, uint32_t bb, uint32_t be) {
  return ab < be && bb < ae;
}
static int binding_unit_conflicts(const AllocationFunction *af, uint32_t vreg,
                                  uint32_t begin, uint32_t end, uint32_t unit) {
  if (!af->binding_count)
    return 0;
  for (uint32_t u = 0; u < af->liveness.vreg_count; ++u) {
    if (u == vreg || !af->liveness.intervals[u].valid || !af->binding_units ||
        af->binding_units[u] != unit)
      continue;
    if (segment_overlap(begin, end, af->liveness.intervals[u].first_slot,
                        af->liveness.intervals[u].last_slot + 1u))
      return 1;
  }
  return 0;
}

static void
rebuild_register_busy_until(const AllocationFunction *af, uint32_t slot,
                            uint32_t busy_until[MACHINE_REGISTER_UNITS]) {
  memset(busy_until, 0, MACHINE_REGISTER_UNITS * sizeof(*busy_until));
  for (uint32_t v = 0; v < af->liveness.vreg_count; ++v)
    for (uint32_t s = 0; s < regalloc_location_piece_count(af, v); ++s) {
      const RegAllocLocationPiece *piece = &af->location_pieces[v][s];
      if (piece->location.kind != AST_REGALLOC_LOCATION_REGISTER ||
          piece->begin > slot || piece->end <= slot ||
          piece->location.register_unit >= MACHINE_REGISTER_UNITS)
        continue;
      uint32_t unit = piece->location.register_unit;
      if (piece->end > busy_until[unit])
        busy_until[unit] = piece->end;
    }
}

static int unit_range_compare(const void *a, const void *b) {
  const RegAllocUnitRange *x = (const RegAllocUnitRange *)a;
  const RegAllocUnitRange *y = (const RegAllocUnitRange *)b;
  if (x->begin != y->begin)
    return x->begin < y->begin ? -1 : 1;
  if (x->end != y->end)
    return x->end < y->end ? -1 : 1;
  return 0;
}

/* Build a compact inverse liveness index once.  Previous experiments queried
   liveness by scanning every virtual interval at each scratch borrow, which
   made scratch-heavy RV64/x86 lowering noticeably slower.  Grouping final
   register pieces by alias unit makes the hot query O(log ranges-on-unit). */
static int build_unit_live_index(AllocationFunction *af) {
  if (!af)
    return 0;
  uint32_t counts[MACHINE_REGISTER_UNITS] = {0};
  uint32_t cursors[MACHINE_REGISTER_UNITS] = {0};
  size_t total = 0u;
  for (uint32_t v = 0; v < af->liveness.vreg_count; ++v)
    for (uint32_t s = 0; s < regalloc_location_piece_count(af, v); ++s) {
      const RegAllocLocationPiece *piece = &af->location_pieces[v][s];
      if (piece->location.kind != AST_REGALLOC_LOCATION_REGISTER)
        continue;
      uint32_t unit = piece->location.register_unit;
      if (unit >= MACHINE_REGISTER_UNITS || counts[unit] == UINT32_MAX)
        return 0;
      ++counts[unit];
      ++total;
    }
  if (total > UINT32_MAX)
    return 0;
  af->unit_range_offsets = AST_ARENA_ARRAY(&af->allocation_storage, uint32_t,
                                           MACHINE_REGISTER_UNITS + 1u);
  af->unit_range_storage = AST_ARENA_ARRAY(
      &af->allocation_storage, RegAllocUnitRange, total ? total : 1u);
  if (!af->unit_range_offsets || !af->unit_range_storage)
    return 0;
  uint32_t off = 0u;
  for (uint32_t unit = 0; unit < MACHINE_REGISTER_UNITS; ++unit) {
    af->unit_range_offsets[unit] = off;
    cursors[unit] = off;
    if (counts[unit] > UINT32_MAX - off)
      return 0;
    off += counts[unit];
  }
  af->unit_range_offsets[MACHINE_REGISTER_UNITS] = off;
  for (uint32_t v = 0; v < af->liveness.vreg_count; ++v)
    for (uint32_t s = 0; s < regalloc_location_piece_count(af, v); ++s) {
      const RegAllocLocationPiece *piece = &af->location_pieces[v][s];
      if (piece->location.kind != AST_REGALLOC_LOCATION_REGISTER)
        continue;
      uint32_t unit = piece->location.register_unit;
      af->unit_range_storage[cursors[unit]++] =
          (RegAllocUnitRange){piece->begin, piece->end};
    }
  for (uint32_t unit = 0; unit < MACHINE_REGISTER_UNITS; ++unit) {
    uint32_t begin = af->unit_range_offsets[unit], count = counts[unit];
    if (count > 1u)
      qsort(af->unit_range_storage + begin, count,
            sizeof(*af->unit_range_storage), unit_range_compare);
  }
  return 1;
}

static int segment_crosses_call(const AllocationFunction *af, uint32_t begin,
                                uint32_t end) {
  uint32_t lo = 0u, hi = af->call_count;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2u;
    if (af->call_slots[mid] < begin)
      lo = mid + 1u;
    else
      hi = mid;
  }
  return lo < af->call_count && af->call_slots[lo] + 1u < end;
}
static void set_all_piece_locations(AllocationFunction *af, uint32_t vreg,
                                    const AstRegAllocLocation *location) {
  uint32_t count = regalloc_location_piece_count(af, vreg);
  for (uint32_t s = 0; s < count; ++s)
    af->location_pieces[vreg][s].location = *location;
}

/* Pick a live, already assigned interval that is cheaper to spill than the
   segment currently being allocated. Spilling all of the victim's pieces is
   conservative but keeps its value representation uniform for edge copies,
   rematerialization and frame-slot verification. */
static uint32_t segment_eviction_candidate(const AllocationFunction *af,
                                           uint32_t current_vreg,
                                           uint32_t begin, uint32_t end,
                                           uint64_t current_weight) {
  uint32_t victim = AST_MACHINE_INVALID_ID;
  uint64_t victim_weight = current_weight;
  for (uint32_t u = 0; u < af->liveness.vreg_count; ++u) {
    if (u == current_vreg || af->binding_units[u] != AST_MACHINE_INVALID_ID ||
        !af->liveness.intervals[u].valid ||
        af->liveness.intervals[u].weight >= victim_weight)
      continue;
    for (uint32_t piece = 0; piece < regalloc_location_piece_count(af, u);
         ++piece) {
      const RegAllocLocationPiece *candidate = &af->location_pieces[u][piece];
      if (candidate->location.kind == AST_REGALLOC_LOCATION_REGISTER &&
          segment_overlap(begin, end, candidate->begin, candidate->end)) {
        victim = u;
        victim_weight = af->liveness.intervals[u].weight;
        break;
      }
    }
  }
  return victim;
}

static int x86_destructive_affinity(const MachineInstruction *mi,
                                    uint32_t *primary, uint32_t *alternate) {
  if (!mi || !primary || !alternate || mi->info.result_count != 1u ||
      mi->info.result_first == AST_MACHINE_INVALID_ID)
    return 0;
  *primary = *alternate = AST_MACHINE_INVALID_ID;
  int unary = mi->info.opcode == AST_MACHINE_X86_NEG ||
              (mi->info.opcode == AST_MACHINE_X86_NOT &&
               mi->info.token_op != AST_TOK_BANG);
  int binary = mi->info.opcode == AST_MACHINE_X86_ADD ||
               mi->info.opcode == AST_MACHINE_X86_SUB ||
               mi->info.opcode == AST_MACHINE_X86_MUL ||
               mi->info.opcode == AST_MACHINE_X86_AND ||
               mi->info.opcode == AST_MACHINE_X86_OR ||
               mi->info.opcode == AST_MACHINE_X86_XOR ||
               mi->info.opcode == AST_MACHINE_X86_FADD ||
               mi->info.opcode == AST_MACHINE_X86_FSUB ||
               mi->info.opcode == AST_MACHINE_X86_FMUL ||
               mi->info.opcode == AST_MACHINE_X86_FDIV;
  if ((!unary && !binary) || mi->info.operand_count != (unary ? 1u : 2u) ||
      mi->info.operands[0].kind != AST_MACHINE_OPERAND_VREG)
    return 0;
  *primary = mi->info.operands[0].as.vreg;
  if (binary && mi->info.operands[1].kind == AST_MACHINE_OPERAND_VREG &&
      (mi->info.opcode == AST_MACHINE_X86_ADD ||
       mi->info.opcode == AST_MACHINE_X86_MUL ||
       mi->info.opcode == AST_MACHINE_X86_AND ||
       mi->info.opcode == AST_MACHINE_X86_OR ||
       mi->info.opcode == AST_MACHINE_X86_XOR))
    *alternate = mi->info.operands[1].as.vreg;
  return 1;
}

static const AstRegAllocLocation *
affinity_source_location(const AllocationFunction *af, uint32_t source,
                         uint32_t begin) {
  if (!af || source >= af->liveness.vreg_count)
    return NULL;
  const AstRegAllocLocation *loc = regalloc_location_at(af, source, begin);
  if ((!loc || loc->kind != AST_REGALLOC_LOCATION_REGISTER) && begin)
    loc = regalloc_location_at(af, source, begin - 1u);
  return loc;
}

/* Segment-aware linear scan. CFG edges are rewritten later, so a value may
   change homes only between liveness segments and never while it is live. */
static int regalloc_function_split(AstRegAllocProgram *p, uint32_t f) {
  AllocationFunction *af = &p->functions[f];
  const MachineFunction *mf = &p->machine->functions[f];
  uint32_t n = mf->info.vreg_count;
  AllocationSegmentOrder *order = NULL;
  uint8_t *spilled = NULL;
  uint32_t *assigned = NULL;
  AstArena scratch = {0};
  uint32_t busy_until[MACHINE_REGISTER_UNITS] = {0};
  size_t segment_total = 0u;
  af->copy_temp = af->abi_temp = af->sret_home = AST_MACHINE_INVALID_ID;
  if (!machine_liveness_build(mf, &af->liveness))
    return regalloc_fail(
        p, AST_REGALLOC_DIAG_INVALID_LIVENESS, f, AST_MACHINE_INVALID_ID,
        "Malformed machine CFG or unrepresentable live intervals.");
  /* Most allocator metadata has exactly function lifetime.  One arena
     replaces the previous 8+ heap allocations per function while retaining
     ordinary realloc-backed frame/physical streams that can outgrow hints. */
  size_t metadata_per_vreg =
      sizeof(AstRegAllocLocation) + sizeof(RegAllocLocationPiece *) +
      sizeof(uint32_t) * 6u + sizeof(RegAllocLocationPiece);
  size_t metadata_hint = 1024u, add = 0u;
  if (ast_size_mul_overflow((size_t)(n ? n : 1u), metadata_per_vreg, &add) ||
      ast_size_add_overflow(metadata_hint, add, &metadata_hint) ||
      ast_size_mul_overflow((size_t)(mf->info.instruction_count
                                         ? mf->info.instruction_count
                                         : 1u),
                            sizeof(uint32_t), &add) ||
      ast_size_add_overflow(metadata_hint, add, &metadata_hint))
    goto oom;
  ast_arena_init(&af->allocation_storage, metadata_hint);
  af->locations =
      AST_ARENA_ARRAY(&af->allocation_storage, AstRegAllocLocation, n ? n : 1u);
  af->location_pieces = AST_ARENA_ARRAY(&af->allocation_storage,
                                        RegAllocLocationPiece *, n ? n : 1u);
  af->location_piece_counts =
      AST_ARENA_ARRAY(&af->allocation_storage, uint32_t, n ? n : 1u);
  af->copy_sources =
      AST_ARENA_ARRAY(&af->allocation_storage, uint32_t, n ? n : 1u);
  af->affinity_sources =
      AST_ARENA_ARRAY(&af->allocation_storage, uint32_t, n ? n : 1u);
  af->alternate_affinity_sources =
      AST_ARENA_ARRAY(&af->allocation_storage, uint32_t, n ? n : 1u);
  af->abi_preferred_units =
      AST_ARENA_ARRAY(&af->allocation_storage, uint32_t, n ? n : 1u);
  af->binding_units =
      AST_ARENA_ARRAY(&af->allocation_storage, uint32_t, n ? n : 1u);
  af->call_slots = AST_ARENA_ARRAY(
      &af->allocation_storage, uint32_t,
      mf->info.instruction_count ? mf->info.instruction_count : 1u);
  if (!af->locations || !af->location_pieces || !af->location_piece_counts ||
      !af->copy_sources || !af->affinity_sources ||
      !af->alternate_affinity_sources || !af->abi_preferred_units ||
      !af->binding_units || !af->call_slots)
    goto oom;
  memset(af->locations, 0, (size_t)(n ? n : 1u) * sizeof(*af->locations));
  memset(af->location_pieces, 0,
         (size_t)(n ? n : 1u) * sizeof(*af->location_pieces));
  memset(af->location_piece_counts, 0,
         (size_t)(n ? n : 1u) * sizeof(*af->location_piece_counts));
  /* Block traversal matches slot numbering, so the call index is sorted. */
  for (uint32_t b = 0; b < mf->info.block_count; ++b)
    for (uint32_t j = 0; j < mf->blocks[b].info.instruction_count; ++j) {
      uint32_t id = mf->blocks[b].instructions[j];
      AstMirOp op = mf->instructions[id].info.source_op;
      if (op == AST_MIR_OP_CALL_DIRECT || op == AST_MIR_OP_CALL_INDIRECT)
        af->call_slots[af->call_count++] = af->liveness.instruction_slots[id];
    }
  for (uint32_t v = 0; v < n; ++v) {
    af->copy_sources[v] = AST_MACHINE_INVALID_ID;
    af->affinity_sources[v] = AST_MACHINE_INVALID_ID;
    af->alternate_affinity_sources[v] = AST_MACHINE_INVALID_ID;
    af->abi_preferred_units[v] = AST_MACHINE_INVALID_ID;
    af->binding_units[v] = AST_MACHINE_INVALID_ID;
    af->locations[v].bank = mf->vregs[v].bank;
    af->locations[v].bit_width = mf->vregs[v].bit_width;
    af->locations[v].frame_object = AST_MACHINE_INVALID_ID;
    af->locations[v].register_unit = AST_MACHINE_INVALID_ID;
    if ((mf->vregs[v].flags & AST_MACHINE_VREG_FUNCTION_ARGUMENT) &&
        mf->vregs[v].fixed_register[0]) {
      MachinePhysRegView incoming;
      if (machine_physreg_view(&p->target, mf->vregs[v].fixed_register,
                               &incoming) &&
          incoming.alias_unit < MACHINE_REGISTER_UNITS)
        af->abi_preferred_units[v] = incoming.alias_unit;
    }
    uint32_t count = af->liveness.intervals[v].segment_count;
    af->location_piece_counts[v] = count;
    if (segment_total > SIZE_MAX - count)
      goto oom;
    segment_total += count;
    p->stats.live_segments += count;
  }
  /* Allocation-order and bookkeeping arrays are temporary.  One scratch
     arena replaces three libc allocations per function and is released as
     a unit immediately after linear scan finishes. */
  size_t scratch_hint = 256u, scratch_bytes = 0u;
  if (ast_size_mul_overflow((size_t)(n ? n : 1u),
                            sizeof(*spilled) + sizeof(*assigned),
                            &scratch_bytes) ||
      ast_size_add_overflow(scratch_hint, scratch_bytes, &scratch_hint) ||
      ast_size_mul_overflow(segment_total ? segment_total : 1u, sizeof(*order),
                            &scratch_bytes) ||
      ast_size_add_overflow(scratch_hint, scratch_bytes, &scratch_hint))
    goto oom;
  ast_arena_init(&scratch, scratch_hint);
  spilled = (uint8_t *)ast_arena_calloc(&scratch, n ? n : 1u, sizeof(*spilled),
                                        ASTER_ALIGNOF(uint8_t));
  assigned = (uint32_t *)ast_arena_calloc(
      &scratch, n ? n : 1u, sizeof(*assigned), ASTER_ALIGNOF(uint32_t));
  order = AST_ARENA_ARRAY(&scratch, AllocationSegmentOrder,
                          segment_total ? segment_total : 1u);
  if (!spilled || !assigned || !order)
    goto oom;
  if (segment_total) {
    size_t bytes;
    if (ast_size_mul_overflow(segment_total,
                              sizeof(*af->location_piece_storage), &bytes))
      goto oom;
    af->location_piece_storage = (RegAllocLocationPiece *)ast_arena_calloc(
        &af->allocation_storage, 1u, bytes,
        ASTER_ALIGNOF(RegAllocLocationPiece));
    if (!af->location_piece_storage)
      goto oom;
  }
  size_t piece_cursor = 0u;
  for (uint32_t v = 0; v < n; ++v) {
    uint32_t count = af->location_piece_counts[v];
    if (!count)
      continue;
    af->location_pieces[v] = af->location_piece_storage + piece_cursor;
    piece_cursor += count;
    for (uint32_t piece = 0; piece < count; ++piece) {
      af->location_pieces[v][piece].begin =
          af->liveness.intervals[v].segments[piece].begin;
      af->location_pieces[v][piece].end =
          af->liveness.intervals[v].segments[piece].end;
      af->location_pieces[v][piece].location.bank = af->locations[v].bank;
      af->location_pieces[v][piece].location.bit_width =
          af->locations[v].bit_width;
      af->location_pieces[v][piece].location.frame_object =
          AST_MACHINE_INVALID_ID;
      af->location_pieces[v][piece].location.register_unit =
          AST_MACHINE_INVALID_ID;
    }
  }
  for (uint32_t i = 0; i < mf->info.instruction_count; ++i) {
    const MachineInstruction *mi = &mf->instructions[i];
    if (mi->info.source_op == AST_MIR_OP_BIND) {
      uint32_t v;
      MachinePhysRegView view;
      if (!bind_view(p, f, mi, &v, &view) ||
          (af->binding_units[v] != AST_MACHINE_INVALID_ID &&
           af->binding_units[v] != view.alias_unit)) {
        regalloc_fail(p, AST_REGALLOC_DIAG_IMPOSSIBLE_CONSTRAINT, f, i,
                      "Bind requires one non-address-taken SSA value and a "
                      "compatible allocatable register or ABI role.");
        goto fail;
      }
      if (af->binding_units[v] == AST_MACHINE_INVALID_ID)
        ++af->binding_count;
      af->binding_units[v] = view.alias_unit;
    }
    /* A value used only at one ABI boundary can be produced directly in
       that boundary's register. Interference and call preservation still
       decide whether this preference is legal and profitable. */
    for (uint32_t o = 0; o < mi->info.operand_count; ++o) {
      const AstMachineOperand *operand = &mi->info.operands[o];
      if (operand->kind != AST_MACHINE_OPERAND_VREG ||
          !(operand->flags & AST_MACHINE_OPERAND_FIXED_REGISTER) ||
          operand->as.vreg >= n)
        continue;
      uint32_t value = operand->as.vreg;
      MachinePhysRegView view;
      if (!(mf->vregs[value].flags & AST_MACHINE_VREG_FUNCTION_ARGUMENT) &&
          af->liveness.intervals[value].use_count == 1u &&
          machine_physreg_view(&p->target, operand->fixed_register, &view))
        af->abi_preferred_units[value] = view.alias_unit;
    }
    if (mi->info.source_op == AST_MIR_OP_COPY && mi->info.result_count == 1u &&
        mi->info.operand_count == 1u &&
        mi->info.operands[0].kind == AST_MACHINE_OPERAND_VREG)
      af->copy_sources[mi->info.result_first] = mi->info.operands[0].as.vreg;
    if (p->target.arch == AST_TARGET_ARCH_X86_64) {
      uint32_t primary, alternate;
      if (x86_destructive_affinity(mi, &primary, &alternate) &&
          mi->info.result_first < n) {
        /* Byte multiply is widened into scratch 32-bit registers by
           physical lowering, so tying its byte result is not useful. */
        if (!(mi->info.opcode == AST_MACHINE_X86_MUL &&
              mf->vregs[mi->info.result_first].bit_width == 8u)) {
          af->affinity_sources[mi->info.result_first] = primary;
          af->alternate_affinity_sources[mi->info.result_first] = alternate;
        }
      }
    }
  }
  /* Carry a single-consumer destination preference backwards through copy
     and destructive arithmetic chains, so allocation need not introduce a
     temporary immediately before a return. Never merge live intervals or
     weaken their interference checks. */
  if (p->config.enable_coalescing)
    for (uint32_t rev = n; rev; --rev) {
      uint32_t v = rev - 1u, source = af->copy_sources[v];
      if (source == AST_MACHINE_INVALID_ID)
        source = af->affinity_sources[v];
      if (source < n && af->abi_preferred_units[v] != AST_MACHINE_INVALID_ID &&
          af->abi_preferred_units[source] == AST_MACHINE_INVALID_ID &&
          !(mf->vregs[source].flags & AST_MACHINE_VREG_FUNCTION_ARGUMENT) &&
          af->liveness.intervals[source].use_count == 1u &&
          mf->vregs[source].bank == mf->vregs[v].bank &&
          mf->vregs[source].bit_width == mf->vregs[v].bit_width)
        af->abi_preferred_units[source] = af->abi_preferred_units[v];
    }
  /* A single-use integer constant is cheaper to keep as a rematerializable
     immediate than to occupy a physical register for its only consumer.
     Consumers that require a register still materialize it locally, while
     immediate-capable x86 forms can consume it directly. */
  for (uint32_t v = 0; v < n; ++v) {
    if (rematerialize_single_use_constant(p, f, v))
      spilled[v] = 1u;
  }
  if (segment_total > UINT32_MAX)
    goto oom;
  size_t order_cursor = 0u;
  for (uint32_t v = 0; v < n; ++v)
    for (uint32_t s = 0; s < af->location_piece_counts[v]; ++s)
      order[order_cursor++] =
          (AllocationSegmentOrder){v, s, af->location_pieces[v][s].begin,
                                   af->liveness.intervals[v].weight};
  qsort(order, order_cursor, sizeof(*order), segment_order_compare);
  for (size_t oi = 0; oi < order_cursor; ++oi) {
    uint32_t v = order[oi].vreg, s = order[oi].piece;
    if (spilled[v])
      continue;
    AstRegAllocLocation *loc = &af->location_pieces[v][s].location;
    uint32_t begin = af->location_pieces[v][s].begin,
             end = af->location_pieces[v][s].end;
    if (!loc->bit_width) {
      regalloc_fail(p, AST_REGALLOC_DIAG_UNSUPPORTED_VALUE, f,
                    AST_MACHINE_INVALID_ID,
                    "Scalable values need a scalable register/frame contract.");
      goto fail;
    }
    const RegAllocRegisterSet *set = register_set(p, loc->bank, loc->bit_width);
    if (!set)
      goto oom;
    uint32_t best_cost = UINT32_MAX, best_unit = AST_MACHINE_INVALID_ID;
    char best_name[AST_MACHINE_REGISTER_NAME_CAP] = {0};
    uint32_t accepted = 0u;
    int bound = af->binding_units[v] != AST_MACHINE_INVALID_ID;
    for (uint32_t r = 0; r < set->count; ++r) {
      const RegisterCandidate *candidate = &set->candidates[r];
      uint32_t unit = candidate->unit;
      if (bound && af->binding_units[v] != unit)
        continue;
      if (!bound && p->config.register_limit &&
          accepted >= p->config.register_limit)
        break;
      ++accepted;
      if ((unit < MACHINE_REGISTER_UNITS && busy_until[unit] > begin) ||
          binding_unit_conflicts(af, v, begin, end, unit))
        continue;
      uint32_t preserved = candidate->preserved;
      uint32_t cost = segment_crosses_call(af, begin, end)
                          ? (preserved >= loc->bit_width ? 0u : 8u)
                          : (preserved ? 2u : 0u);
      /* Keep a short-lived function argument in its incoming ABI
         register when that is otherwise equally good.  This is an
         affinity, not a hard constraint: an argument crossing a call
         still prefers a callee-saved register, and real interference
         can choose any legal home.  Avoiding the entry permutation is
         especially valuable for leaf helpers such as VGA/serial I/O. */
      uint32_t incoming = af->abi_preferred_units[v];
      if (incoming != AST_MACHINE_INVALID_ID &&
          (s == 0u ||
           !(mf->vregs[v].flags & AST_MACHINE_VREG_FUNCTION_ARGUMENT))) {
        if (unit == incoming)
          cost = cost >= 2u ? cost - 2u : 0u;
        else if (cost <= UINT32_MAX - 2u)
          cost += 2u;
      }
      uint32_t source = af->copy_sources[v];
      const AstRegAllocLocation *source_loc =
          affinity_source_location(af, source, begin);
      if (p->config.enable_coalescing && source_loc &&
          source_loc->kind == AST_REGALLOC_LOCATION_REGISTER &&
          source_loc->register_unit == unit) {
        cost = 0u;
      } else {
        uint32_t primary = af->affinity_sources[v],
                 alternate = af->alternate_affinity_sources[v];
        const AstRegAllocLocation *primary_loc =
            affinity_source_location(af, primary, begin);
        const AstRegAllocLocation *alternate_loc =
            affinity_source_location(af, alternate, begin);
        if (primary_loc &&
            primary_loc->kind == AST_REGALLOC_LOCATION_REGISTER &&
            primary_loc->register_unit == unit) {
          /* x86 scalar arithmetic is destructive.  Reusing lhs
             removes the preparatory MOV without weakening ABI
             preserved-register preferences. */
        } else if (alternate_loc &&
                   alternate_loc->kind == AST_REGALLOC_LOCATION_REGISTER &&
                   alternate_loc->register_unit == unit)
          cost += 1u;
        else
          cost += 2u;
      }
      if (cost < best_cost) {
        best_cost = cost;
        best_unit = unit;
        memcpy(best_name, candidate->name, sizeof(best_name));
      }
    }
    if (best_unit == AST_MACHINE_INVALID_ID) {
      if (bound) {
        regalloc_fail(
            p, AST_REGALLOC_DIAG_IMPOSSIBLE_CONSTRAINT, f,
            AST_MACHINE_INVALID_ID,
            "Overlapping bind constraints require the same register unit.");
        goto fail;
      }
      uint32_t victim = segment_eviction_candidate(
          af, v, begin, end, af->liveness.intervals[v].weight);
      if (victim != AST_MACHINE_INVALID_ID) {
        AstRegAllocLocation *victim_location = &af->locations[victim];
        if (!spill_value(p, f, victim))
          goto fail;
        if (assigned[victim] <= p->stats.assigned_registers)
          p->stats.assigned_registers -= assigned[victim];
        assigned[victim] = 0u;
        spilled[victim] = 1u;
        set_all_piece_locations(af, victim, victim_location);
        rebuild_register_busy_until(af, begin, busy_until);
        if (!oi)
          goto fail;
        --oi;
        continue;
      }
      AstRegAllocLocation *canonical = &af->locations[v];
      if (!spill_value(p, f, v))
        goto fail;
      if (assigned[v] <= p->stats.assigned_registers)
        p->stats.assigned_registers -= assigned[v];
      assigned[v] = 0u;
      spilled[v] = 1u;
      set_all_piece_locations(af, v, canonical);
      continue;
    }
    loc->kind = AST_REGALLOC_LOCATION_REGISTER;
    loc->register_unit = best_unit;
    memcpy(loc->register_name, best_name, sizeof(best_name));
    if (best_unit < MACHINE_REGISTER_UNITS && end > busy_until[best_unit])
      busy_until[best_unit] = end;
    ++assigned[v];
    ++p->stats.assigned_registers;
    uint32_t source = af->copy_sources[v];
    const AstRegAllocLocation *source_loc =
        affinity_source_location(af, source, begin);
    if (source_loc && source_loc->kind == AST_REGALLOC_LOCATION_REGISTER &&
        source_loc->register_unit == best_unit)
      ++p->stats.coalesced_copies;
  }
  for (uint32_t v = 0; v < n; ++v) {
    if (!af->liveness.intervals[v].valid)
      continue;
    if (!spilled[v] && af->location_piece_counts[v])
      af->locations[v] = af->location_pieces[v][0].location;
  }
  if (!build_unit_live_index(af))
    goto oom;
  ast_arena_destroy(&scratch);
  return 1;
oom:
  regalloc_fail(p, AST_REGALLOC_DIAG_OUT_OF_MEMORY, f, AST_MACHINE_INVALID_ID,
                "Cannot allocate register allocation storage.");
fail:
  ast_arena_destroy(&scratch);
  return 0;
}
typedef struct VerifyPieceOrder {
  const RegAllocLocationPiece *piece;
} VerifyPieceOrder;

static int verify_piece_order_compare(const void *a, const void *b) {
  const RegAllocLocationPiece *x = ((const VerifyPieceOrder *)a)->piece;
  const RegAllocLocationPiece *y = ((const VerifyPieceOrder *)b)->piece;
  if (x->begin != y->begin)
    return x->begin < y->begin ? -1 : 1;
  if (x->end != y->end)
    return x->end < y->end ? -1 : 1;
  return 0;
}

int regalloc_verify_function(const AstRegAllocProgram *p, uint32_t f) {
  const MachineFunction *mf = &p->machine->functions[f];
  const AllocationFunction *af = &p->functions[f];
  if (!af->locations || !machine_liveness_verify(mf, &af->liveness))
    return 0;
  for (uint32_t i = 0; i < mf->info.instruction_count; ++i)
    if (mf->instructions[i].info.source_op == AST_MIR_OP_BIND &&
        !regalloc_verify_bind_instruction(p, f, &mf->instructions[i]))
      return 0;
  for (uint32_t v = 0; v < mf->info.vreg_count; ++v) {
    const AstRegAllocLocation *loc = &af->locations[v];
    if (loc->bank != mf->vregs[v].bank ||
        loc->bit_width != mf->vregs[v].bit_width)
      return 0;
    uint32_t count = regalloc_location_piece_count(af, v);
    if (!af->liveness.intervals[v].valid) {
      if (loc->kind != AST_REGALLOC_LOCATION_NONE || count)
        return 0;
      continue;
    }
    if (!count || count != af->liveness.intervals[v].segment_count ||
        !af->location_pieces[v])
      return 0;
    for (uint32_t s = 0; s < count; ++s) {
      const RegAllocLocationPiece *piece = &af->location_pieces[v][s];
      const AstRegAllocLocation *part = &piece->location;
      const MachineLiveSegment *segment =
          &af->liveness.intervals[v].segments[s];
      if (piece->begin != segment->begin || piece->end != segment->end ||
          part->bank != loc->bank || part->bit_width != loc->bit_width)
        return 0;
      if (part->kind == AST_REGALLOC_LOCATION_REGISTER) {
        MachinePhysRegView view;
        if (!memchr(part->register_name, 0, sizeof(part->register_name)) ||
            !machine_physreg_view(&p->target, part->register_name, &view) ||
            allocator_reserved(&p->target, &view) ||
            view.alias_unit != part->register_unit ||
            view.width < part->bit_width ||
            !machine_register_compatible(&p->target, part->register_name,
                                         part->bank))
          return 0;
      } else if (part->kind == AST_REGALLOC_LOCATION_SPILL) {
        if (part->frame_object >= af->object_count ||
            af->objects[part->frame_object].kind != AST_FRAME_OBJECT_SPILL ||
            af->objects[part->frame_object].size <
                ((uint64_t)part->bit_width + 7u) / 8u)
          return 0;
      } else if (part->kind == AST_REGALLOC_LOCATION_REMATERIALIZE) {
        int found = 0;
        for (uint32_t i = 0; i < mf->info.instruction_count; ++i) {
          const MachineInstruction *mi = &mf->instructions[i];
          if (mi->info.source_op == AST_MIR_OP_CONSTANT &&
              mi->info.result_count == 1u && mi->info.result_first == v &&
              mi->info.operand_count == 1u &&
              mi->info.operands[0].kind == AST_MACHINE_OPERAND_IMMEDIATE &&
              (uint64_t)mi->info.operands[0].as.immediate ==
                  part->immediate_bits)
            found = 1;
        }
        if (!found || af->liveness.intervals[v].def_count != 1u ||
            part->bank != AST_MACHINE_BANK_GPR || part->bit_width > 64u)
          return 0;
      } else
        return 0;
    }
    if (loc->kind != af->location_pieces[v][0].location.kind ||
        loc->register_unit !=
            af->location_pieces[v][0].location.register_unit ||
        loc->frame_object != af->location_pieces[v][0].location.frame_object ||
        loc->immediate_bits !=
            af->location_pieces[v][0].location.immediate_bits ||
        strcmp(loc->register_name,
               af->location_pieces[v][0].location.register_name) != 0)
      return 0;
  }
  /* Conflict verification used to compare every live segment with every
     other segment (O(S^2)).  Sort once by start slot and keep the latest
     end per architectural unit / spill object instead.  This proves the
     same no-overlap invariant in O(S log S), which matters for large
     generated functions and for verification-enabled release builds. */
  size_t piece_total = 0u;
  for (uint32_t v = 0; v < mf->info.vreg_count; ++v) {
    uint32_t count = regalloc_location_piece_count(af, v);
    if (piece_total > SIZE_MAX - count)
      return 0;
    piece_total += count;
  }
  if (piece_total <= 32u) {
    for (size_t i = 0; i < piece_total; ++i) {
      const RegAllocLocationPiece *a = &af->location_piece_storage[i];
      for (size_t j = i + 1u; j < piece_total; ++j) {
        const RegAllocLocationPiece *b = &af->location_piece_storage[j];
        if (!segment_overlap(a->begin, a->end, b->begin, b->end))
          continue;
        if (a->location.kind == AST_REGALLOC_LOCATION_REGISTER &&
            b->location.kind == AST_REGALLOC_LOCATION_REGISTER &&
            a->location.register_unit == b->location.register_unit)
          return 0;
        if (a->location.kind == AST_REGALLOC_LOCATION_SPILL &&
            b->location.kind == AST_REGALLOC_LOCATION_SPILL &&
            a->location.frame_object == b->location.frame_object)
          return 0;
      }
    }
    return 1;
  }
  VerifyPieceOrder *ordered =
      (VerifyPieceOrder *)malloc(piece_total * sizeof(*ordered));
  uint32_t *spill_end =
      af->object_count
          ? (uint32_t *)calloc(af->object_count, sizeof(*spill_end))
          : NULL;
  if ((piece_total && !ordered) || (af->object_count && !spill_end)) {
    free(ordered);
    free(spill_end);
    return 0;
  }
  size_t cursor = 0u;
  for (uint32_t v = 0; v < mf->info.vreg_count; ++v)
    for (uint32_t q = 0; q < regalloc_location_piece_count(af, v); ++q)
      ordered[cursor++].piece = &af->location_pieces[v][q];
  qsort(ordered, piece_total, sizeof(*ordered), verify_piece_order_compare);
  uint32_t register_end[MACHINE_REGISTER_UNITS] = {0};
  int valid = 1;
  for (size_t i = 0; i < piece_total && valid; ++i) {
    const RegAllocLocationPiece *piece = ordered[i].piece;
    if (piece->location.kind == AST_REGALLOC_LOCATION_REGISTER) {
      uint32_t unit = piece->location.register_unit;
      if (unit >= MACHINE_REGISTER_UNITS || register_end[unit] > piece->begin)
        valid = 0;
      else if (piece->end > register_end[unit])
        register_end[unit] = piece->end;
    } else if (piece->location.kind == AST_REGALLOC_LOCATION_SPILL) {
      uint32_t object = piece->location.frame_object;
      if (object >= af->object_count || spill_end[object] > piece->begin)
        valid = 0;
      else if (piece->end > spill_end[object])
        spill_end[object] = piece->end;
    }
  }
  free(spill_end);
  free(ordered);
  return valid;
}

typedef struct RegAllocParallelContext {
  AstRegAllocProgram *program;
  AstRegAllocProgram *results;
  RegAllocFunctionFn lower;
  void *context;
} RegAllocParallelContext;
static int allocation_task(void *opaque, size_t index) {
  RegAllocParallelContext *ctx = opaque;
  AstRegAllocProgram *local = &ctx->results[index];
  *local = *ctx->program;
  memset(&local->stats, 0, sizeof(local->stats));
  memset(&local->diagnostic, 0, sizeof(local->diagnostic));
  int ok = ctx->lower(local, (uint32_t)index, ctx->context);
  if (local->stats.error_count && local->diagnostic.message &&
      local->diagnostic.message != local->diagnostic_storage)
    snprintf(local->diagnostic_storage, sizeof(local->diagnostic_storage), "%s",
             local->diagnostic.message);
  /* Cache entries are immutable after publication. A rare missing width is
     private to this job and cannot change the shared cache head. */
  while (local->register_sets != ctx->program->register_sets) {
    RegAllocRegisterSet *next = local->register_sets->next;
    free(local->register_sets);
    local->register_sets = next;
  }
  return ok;
}
int regalloc_parallel_functions(AstRegAllocProgram *p, RegAllocFunctionFn lower,
                                void *context) {
  AstRegAllocProgram *results =
      calloc(p->function_count ? p->function_count : 1u, sizeof(*results));
  if (!results)
    return regalloc_fail(p, AST_REGALLOC_DIAG_OUT_OF_MEMORY, 0u,
                         AST_MACHINE_INVALID_ID,
                         "Cannot allocate function job results.");
  RegAllocParallelContext ctx = {p, results, lower, context};
  size_t first;
  int ok =
      ast_parallel_check(p->function_count, 0u, allocation_task, &ctx, &first);
  for (size_t f = 0; f < p->function_count; ++f) {
    AstRegAllocProgram *r = &results[f];
    p->stats.assigned_registers += r->stats.assigned_registers;
    p->stats.spilled_values += r->stats.spilled_values;
    p->stats.rematerialized_values += r->stats.rematerialized_values;
    p->stats.coalesced_copies += r->stats.coalesced_copies;
    p->stats.live_segments += r->stats.live_segments;
    p->stats.frame_bytes += r->stats.frame_bytes;
    p->stats.physical_instructions += r->stats.physical_instructions;
    p->stats.edge_blocks += r->stats.edge_blocks;
    if (r->stats.error_count && !p->stats.error_count) {
      memcpy(p->diagnostic_storage, r->diagnostic_storage,
             sizeof(p->diagnostic_storage));
      regalloc_fail(p, r->diagnostic.code, r->diagnostic.function_index,
                    r->diagnostic.instruction, p->diagnostic_storage);
    }
  }
  free(results);
  if (!ok && !p->stats.error_count)
    regalloc_fail(p, AST_REGALLOC_DIAG_OUT_OF_MEMORY, (uint32_t)first,
                  AST_MACHINE_INVALID_ID, "Function job failed.");
  return ok;
}
int regalloc_allocate_function(AstRegAllocProgram *p, uint32_t f) {
  if (!regalloc_function_split(p, f))
    return 0;
  if (p->config.verify_allocation && !regalloc_verify_function(p, f))
    return regalloc_fail(p, AST_REGALLOC_DIAG_INVALID_LIVENESS, f,
                         AST_MACHINE_INVALID_ID,
                         "Allocation verification failed.");
  return 1;
}
AstRegAllocProgram *regalloc_prepare(const AstMachineProgram *machine,
                                     const AstRegAllocConfig *config) {
  if (!machine)
    return NULL;
  AstRegAllocProgram *p = (AstRegAllocProgram *)calloc(1u, sizeof(*p));
  if (!p)
    return NULL;
  p->machine = machine;
  p->config = config ? *config : ast_regalloc_config_default();
  p->function_count = machine->function_count;
  p->stats.function_count = p->function_count;
  p->target = ast_target_config(machine->target);
  p->abi = ast_abi_config(machine->abi).kind;
  p->functions = (AllocationFunction *)calloc(
      p->function_count ? p->function_count : 1u, sizeof(*p->functions));
  if (!p->functions) {
    free(p);
    return NULL;
  }
  if (machine->stage != AST_MACHINE_STAGE_SELECTED ||
      (p->config.verify_machine && !ast_machine_verify(machine))) {
    regalloc_fail(p, AST_REGALLOC_DIAG_INVALID_MACHINE, 0u,
                  AST_MACHINE_INVALID_ID,
                  "Register allocation requires verified selected Machine IR.");
    return p;
  }
  if (p->target.arch != AST_TARGET_ARCH_X86_64 &&
      p->target.arch != AST_TARGET_ARCH_AARCH64 &&
      p->target.arch != AST_TARGET_ARCH_RISCV64) {
    regalloc_fail(p, AST_REGALLOC_DIAG_UNSUPPORTED_TARGET, 0u,
                  AST_MACHINE_INVALID_ID,
                  "Target has no register allocation contract.");
    return p;
  }
  for (size_t f = 0; f < p->function_count; ++f)
    for (uint32_t v = 0; v < machine->functions[f].info.vreg_count; ++v) {
      const AstMachineVRegInfo *vr = &machine->functions[f].vregs[v];
      if (vr->bit_width && !register_set(p, vr->bank, vr->bit_width)) {
        regalloc_fail(p, AST_REGALLOC_DIAG_OUT_OF_MEMORY, (uint32_t)f,
                      AST_MACHINE_INVALID_ID, "Cannot prepare register cache.");
        return p;
      }
    }
  return p;
}
static int allocate_task(AstRegAllocProgram *p, uint32_t f, void *context) {
  (void)context;
  return regalloc_allocate_function(p, f);
}
AstRegAllocProgram *ast_regalloc_build(const AstMachineProgram *machine,
                                       const AstRegAllocConfig *config) {
  AstRegAllocProgram *p = regalloc_prepare(machine, config);
  if (!p || p->stats.error_count)
    return p;
  int built = regalloc_parallel_functions(p, allocate_task, NULL);
  p->allocation_verified =
      (uint8_t)(p->config.verify_allocation && !p->stats.error_count && built);
  return p;
}
void regalloc_release_function(AllocationFunction *af) {
  machine_liveness_destroy(&af->liveness);
  ast_arena_destroy(&af->allocation_storage);
  free(af->objects);
  physical_release_function(af);
  free(af->register_object_storage);
  memset(af, 0, sizeof(*af));
}
void ast_regalloc_destroy(AstRegAllocProgram *p) {
  if (!p)
    return;
  if (p->functions)
    for (size_t f = 0; f < p->function_count; ++f) {
      regalloc_release_function(&p->functions[f]);
    }
  while (p->register_sets) {
    RegAllocRegisterSet *next = p->register_sets->next;
    free(p->register_sets);
    p->register_sets = next;
  }
  free(p->functions);
  free(p);
}
int ast_regalloc_verify(const AstRegAllocProgram *p) {
  if (!p || !p->machine || !p->functions || p->stats.error_count ||
      p->function_count != p->machine->function_count)
    return 0;
  for (uint32_t f = 0; f < p->function_count; ++f)
    if (!regalloc_verify_function(p, f))
      return 0;
  return 1;
}
AstRegAllocStats ast_regalloc_stats(const AstRegAllocProgram *p) {
  AstRegAllocStats zero = {0};
  return p ? p->stats : zero;
}
const AstRegAllocDiagnostic *
ast_regalloc_diagnostics(const AstRegAllocProgram *p, size_t *count) {
  if (count)
    *count = p ? p->stats.error_count : 0u;
  return p && p->stats.error_count ? &p->diagnostic : NULL;
}
int ast_regalloc_location(const AstRegAllocProgram *p, size_t f,
                          AstMachineVReg v, AstRegAllocLocation *out) {
  if (!p || !out || p->stats.error_count || f >= p->function_count ||
      v >= p->machine->functions[f].info.vreg_count)
    return 0;
  *out = p->functions[f].locations[v];
  return 1;
}
