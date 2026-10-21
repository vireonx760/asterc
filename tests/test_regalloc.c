#include "allocation_internal.h"
#include "asterasm/asterasm.h"
#include "test_process.h"

typedef struct Fixture {
  AstFrontend *frontend;
  AstSema *sema;
  AstHirProgram *hir;
  AstMirProgram *mir;
  AstTargetProgram *target;
  AstAbiProgram *abi;
  AstMachineProgram *machine;
  AstRegAllocProgram *allocation;
} Fixture;

static void close_fixture(Fixture *f) {
  ast_regalloc_destroy(f->allocation);
  ast_machine_destroy(f->machine);
  ast_abi_destroy(f->abi);
  ast_target_destroy(f->target);
  ast_mir_destroy(f->mir);
  ast_hir_destroy(f->hir);
  ast_sema_destroy(f->sema);
  ast_frontend_destroy(f->frontend);
  memset(f, 0, sizeof(*f));
}
static void print_dump(void *user, const char *text, size_t length) {
  (void)fwrite(text, 1u, length, (FILE *)user);
}
static int open_fixture(Fixture *f, int windows, uint32_t limit, int promote,
                        uint32_t test, uint32_t frame_bytes) {
  char root[4096], entry[4096];
  memset(f, 0, sizeof(*f));
  const char *fixture = test >= 56u   ? "core"
                        : test >= 54u ? "overaligned"
                        : test == 52u ? "native_manual"
                        : test == 53u ? "native_naked"
                        : test >= 40u ? "native_scalar"
                        : test >= 24u ? "x86_scalar"
                                      : "core";
  (void)snprintf(root, sizeof(root), "%s/tests/regalloc_cases/%s",
                 ASTERASM_SOURCE_DIR, fixture);
  (void)snprintf(entry, sizeof(entry), "%s/tests/regalloc_cases/%s/main.aa",
                 ASTERASM_SOURCE_DIR, fixture);
  AstFrontendConfig fc = ast_frontend_config_default();
  fc.thread_count = 1u;
  f->frontend = ast_frontend_create(&fc);
  if (!f->frontend || ast_frontend_add_import_root(f->frontend, root) != 0)
    goto fail;
  const char *entries[] = {entry};
  if (ast_frontend_load(f->frontend, entries, 1u) != 0) {
    size_t count = 0u;
    const AstFrontendDiagnostic *diags =
        ast_frontend_diagnostics(f->frontend, &count);
    for (size_t d = 0; d < count; ++d)
      fprintf(stderr, "frontend: %s\n", diags[d].message);
    goto fail;
  }
  AstSemaConfig sc = ast_sema_config_default();
  sc.thread_count = 1u;
  f->sema = ast_sema_create(f->frontend, &sc);
  if (!f->sema || ast_sema_check(f->sema) != 0) {
    fputs("sema failed\n", stderr);
    size_t count = 0u;
    const AstSemaDiagnostic *diags = ast_sema_diagnostics(f->sema, &count);
    for (size_t d = 0; d < count; ++d)
      fprintf(stderr, "%s\n", diags[d].message);
    goto fail;
  }
  AstHirConfig hc = ast_hir_config_default();
  hc.thread_count = 1u;
  f->hir = ast_hir_build(f->frontend, f->sema, &hc);
  if (!f->hir || !ast_hir_verify(f->hir)) {
    fputs("hir failed\n", stderr);
    goto fail;
  }
  AstMirConfig mc = ast_mir_config_default();
  mc.thread_count = 1u;
  mc.promote_locals = (uint8_t)promote;
  f->mir = ast_mir_build(f->frontend, f->hir, &mc);
  if (!f->mir || !ast_mir_verify(f->mir)) {
    fputs("mir failed\n", stderr);
    goto fail;
  }
  AstTargetConfig tc = ast_target_config_default(AST_TARGET_ARCH_X86_64);
  if (test >= 40u)
    tc.features |= AST_TARGET_FEATURE_X86_POPCNT;
  tc.os = windows ? AST_TARGET_OS_WINDOWS : AST_TARGET_OS_LINUX;
  f->target = ast_target_legalize(f->mir, &tc);
  if (!f->target || !ast_target_verify(f->target)) {
    size_t count = 0u;
    const AstTargetDiagnostic *diags =
        ast_target_diagnostics(f->target, &count);
    for (size_t d = 0; d < count; ++d)
      fprintf(stderr, "target: %s\n", diags[d].message);
    goto fail;
  }
  AstAbiConfig ac = ast_abi_config_default(windows ? AST_ABI_WINDOWS_X64
                                                   : AST_ABI_SYSV_AMD64);
  f->abi = ast_abi_lower(f->target, &ac);
  if (!f->abi || !ast_abi_verify(f->abi)) {
    fputs("abi failed\n", stderr);
    goto fail;
  }
  f->machine = ast_backend_lower(f->abi, NULL);
  if (!f->machine) {
    fputs("machine failed\n", stderr);
    goto fail;
  }
  AstRegAllocConfig rc = ast_regalloc_config_default();
  rc.register_limit = limit;
  f->allocation = ast_regalloc_build(f->machine, &rc);
  if (test >= 56u && f->allocation &&
      !ast_regalloc_stats(f->allocation).error_count) {
    for (uint32_t fn = 0; fn < f->allocation->function_count; ++fn)
      if (frame_add_object(&f->allocation->functions[fn],
                           AST_FRAME_OBJECT_LOCAL, 64u,
                           test == 56u ? 32u : 64u) == AST_MACHINE_INVALID_ID)
        goto fail;
  }
  if (frame_bytes && f->allocation &&
      !ast_regalloc_stats(f->allocation).error_count) {
    for (uint32_t fn = 0; fn < f->allocation->function_count; ++fn)
      if (frame_add_object(&f->allocation->functions[fn],
                           AST_FRAME_OBJECT_LOCAL, frame_bytes,
                           16u) == AST_MACHINE_INVALID_ID)
        goto fail;
  }
  if (!f->allocation || !ast_regalloc_verify(f->allocation) ||
      !ast_regalloc_finalize(f->allocation)) {
    if (f->allocation)
      ast_regalloc_dump(f->allocation, print_dump, stderr);
    goto fail;
  }
  return 1;
fail:
  close_fixture(f);
  return 0;
}
static int name_is(AstName n, const char *s) {
  return n.text && n.span.length == strlen(s) &&
         memcmp(n.text, s, n.span.length) == 0;
}
static uint32_t find_function(const Fixture *f, const char *name) {
  for (uint32_t i = 0; i < f->machine->function_count; ++i)
    if (name_is(f->machine->functions[i].info.name, name))
      return i;
  return AST_MACHINE_INVALID_ID;
}

/* A small test-only physical IR interpreter. It uses actual physical register
   aliases, byte-addressed memory and call-stack bias, independently of the
   allocator's home/interval decisions. It never reads virtual registers. */
#define TEST_MEMORY_SIZE 262144u
typedef struct Execution {
  unsigned char regs[MACHINE_REGISTER_UNITS][64];
  unsigned char memory[TEST_MEMORY_SIZE];
  const Fixture *fixture;
  uint32_t steps;
  uint32_t depth;
} Execution;
static uint64_t get_u64(const unsigned char *p) {
  uint64_t n;
  memcpy(&n, p, sizeof(n));
  return n;
}
static void put_u64(unsigned char *p, uint64_t n) { memcpy(p, &n, sizeof(n)); }
static uint64_t width_mask(uint32_t bits) {
  return bits >= 64u ? UINT64_MAX : (UINT64_C(1) << bits) - 1u;
}
static int64_t signed_value(uint64_t value, uint32_t bits) {
  uint64_t mask = width_mask(bits);
  value &= mask;
  if (value & (UINT64_C(1) << (bits - 1u)))
    return -(int64_t)(~value & mask) - 1;
  return (int64_t)value;
}
static int read_value(Execution *e, const AstPhysicalOperand *o,
                      unsigned char out[64]) {
  memset(out, 0, 64u);
  uint32_t bytes = (o->bit_width + 7u) / 8u;
  if (bytes > 64u)
    return 0;
  if (o->kind == AST_PHYSICAL_OPERAND_IMMEDIATE) {
    put_u64(out, o->immediate_bits);
    return 1;
  }
  if (o->kind == AST_PHYSICAL_OPERAND_REGISTER) {
    memcpy(out, e->regs[o->register_unit], bytes);
    return 1;
  }
  if (o->kind == AST_PHYSICAL_OPERAND_MEMORY) {
    uint64_t base = get_u64(e->regs[o->register_unit]);
    int64_t address = (int64_t)base + o->displacement;
    if (address < 0 || (uint64_t)address > TEST_MEMORY_SIZE ||
        bytes > TEST_MEMORY_SIZE - (uint64_t)address)
      return 0;
    memcpy(out, e->memory + (size_t)address, bytes);
    return 1;
  }
  return 0;
}
static int write_value(Execution *e, const AstPhysicalOperand *o,
                       const unsigned char value[64]) {
  uint32_t bytes = (o->bit_width + 7u) / 8u;
  if (!bytes || bytes > 64u)
    return 0;
  if (o->kind == AST_PHYSICAL_OPERAND_REGISTER) {
    if (o->bank == AST_MACHINE_BANK_GPR && o->bit_width == 32u)
      memset(e->regs[o->register_unit], 0, 8u);
    memcpy(e->regs[o->register_unit], value, bytes);
    return 1;
  }
  if (o->kind == AST_PHYSICAL_OPERAND_MEMORY) {
    uint64_t base = get_u64(e->regs[o->register_unit]);
    int64_t address = (int64_t)base + o->displacement;
    if (address < 0 || (uint64_t)address > TEST_MEMORY_SIZE ||
        bytes > TEST_MEMORY_SIZE - (uint64_t)address)
      return 0;
    memcpy(e->memory + (size_t)address, value, bytes);
    return 1;
  }
  return 0;
}
static int execute_function(Execution *e, uint32_t function) {
  if (function >= e->fixture->allocation->function_count || ++e->depth > 32u)
    return 0;
  const AllocationFunction *fn = &e->fixture->allocation->functions[function];
  uint32_t block = e->fixture->machine->functions[function].info.entry_block,
           index = 0u;
  const uint32_t sp_unit = MACHINE_PHYSREG_FILE_GPR * 64u + 4u;
  for (;;) {
    if (++e->steps > 200000u || block >= fn->block_count ||
        index >= fn->blocks[block].count)
      return 0;
    const AstPhysicalInstructionInfo *i =
        &fn->blocks[block].instructions[index++];
    unsigned char a[64] = {0}, b[64] = {0}, out[64] = {0};
    const AstPhysicalOperand *ops = i->operands;
    uint64_t x = 0u, y = 0u, value = 0u;
    switch (i->opcode) {
    case AST_MACHINE_X86_MOV:
    case AST_MACHINE_X86_MOV_IMM:
    case AST_MACHINE_X86_LOAD:
      if (!read_value(e, &ops[1], out) || !write_value(e, &ops[0], out))
        return 0;
      continue;
    case AST_MACHINE_X86_STORE:
      if (!read_value(e, &ops[0], out) || !write_value(e, &ops[1], out))
        return 0;
      continue;
    case AST_MACHINE_X86_LEA:
      if (ops[1].kind != AST_PHYSICAL_OPERAND_MEMORY)
        return 0;
      put_u64(out, get_u64(e->regs[ops[1].register_unit]) +
                       (uint64_t)ops[1].displacement);
      if (!write_value(e, &ops[0], out))
        return 0;
      continue;
    case AST_MACHINE_X86_BR:
      block = ops[0].block;
      index = 0u;
      continue;
    case AST_MACHINE_X86_COND_BR:
      if (!read_value(e, &ops[0], a))
        return 0;
      block = ops[get_u64(a) ? 1u : 2u].block;
      index = 0u;
      continue;
    case AST_MACHINE_X86_ICMP_BR: {
      if (ops[0].kind != AST_PHYSICAL_OPERAND_IMMEDIATE ||
          !read_value(e, &ops[1], a) || !read_value(e, &ops[2], b))
        return 0;
      x = get_u64(a);
      y = get_u64(b);
      int take = 0;
      switch ((AstMachineOpcode)ops[0].immediate_bits) {
      case AST_MACHINE_X86_ICMP_EQ:
        take = x == y;
        break;
      case AST_MACHINE_X86_ICMP_NE:
        take = x != y;
        break;
      case AST_MACHINE_X86_ICMP_LT:
        take = i->is_signed ? signed_value(x, ops[1].bit_width) <
                                  signed_value(y, ops[2].bit_width)
                            : x < y;
        break;
      case AST_MACHINE_X86_ICMP_LE:
        take = i->is_signed ? signed_value(x, ops[1].bit_width) <=
                                  signed_value(y, ops[2].bit_width)
                            : x <= y;
        break;
      case AST_MACHINE_X86_ICMP_GT:
        take = i->is_signed ? signed_value(x, ops[1].bit_width) >
                                  signed_value(y, ops[2].bit_width)
                            : x > y;
        break;
      case AST_MACHINE_X86_ICMP_GE:
        take = i->is_signed ? signed_value(x, ops[1].bit_width) >=
                                  signed_value(y, ops[2].bit_width)
                            : x >= y;
        break;
      default:
        return 0;
      }
      block = ops[take ? 3u : 4u].block;
      index = 0u;
      continue;
    }
    case AST_MACHINE_X86_RET:
      --e->depth;
      return 1;
    case AST_MACHINE_X86_NOP:
      continue;
    case AST_MACHINE_X86_TRAP:
      return 0;
    case AST_MACHINE_X86_CALL: {
      if (ops[0].kind != AST_PHYSICAL_OPERAND_SYMBOL)
        return 0;
      uint32_t target = AST_MACHINE_INVALID_ID;
      for (uint32_t f = 0; f < e->fixture->machine->function_count; ++f) {
        AstName n = e->fixture->machine->functions[f].info.name;
        if (n.span.length == ops[0].symbol.span.length &&
            memcmp(n.text, ops[0].symbol.text, n.span.length) == 0)
          target = f;
      }
      uint64_t sp = get_u64(e->regs[sp_unit]);
      AstAbiFunctionInfo called;
      if (!ast_abi_function_info(e->fixture->abi, target, &called) ||
          !called.stack_alignment || sp % called.stack_alignment) {
        fputs("interpreter: call-site stack alignment violated\n", stderr);
        return 0;
      }
      put_u64(e->regs[sp_unit], sp - 8u);
      if (!execute_function(e, target) || get_u64(e->regs[sp_unit]) != sp - 8u)
        return 0;
      put_u64(e->regs[sp_unit], sp);
      /* An ABI-compliant callee may destroy all volatile registers.
         Preserve only result locations and nonvolatile widths. */
      for (uint32_t unit = 0; unit < MACHINE_REGISTER_UNITS; ++unit) {
        uint32_t file = unit / 64u, reg = unit % 64u;
        if (unit == sp_unit ||
            (file == MACHINE_PHYSREG_FILE_GPR && (reg == 0u || reg == 2u)) ||
            (file == MACHINE_PHYSREG_FILE_FP_VECTOR && reg < 2u))
          continue;
        MachinePhysRegView view = {0};
        view.file = (MachinePhysRegFile)file;
        view.index = reg;
        uint32_t preserved =
            machine_physreg_preserved_bits(e->fixture->allocation->abi, &view) /
            8u;
        if (preserved < 64u)
          memset(e->regs[unit] + preserved, 0xa5, 64u - preserved);
      }
      continue;
    }
    case AST_MACHINE_X86_CDQ:
    case AST_MACHINE_X86_CQO:
      if (!read_value(e, &ops[1], a))
        return 0;
      x = get_u64(a);
      value = (x & (UINT64_C(1) << (ops[1].bit_width - 1u))) ? UINT64_MAX : 0u;
      break;
    case AST_MACHINE_X86_UDIV:
    case AST_MACHINE_X86_SDIV: {
      if (!read_value(e, &ops[2], a) || !read_value(e, &ops[4], b))
        return 0;
      x = get_u64(a);
      y = get_u64(b);
      if (!y)
        return 0;
      uint64_t quotient, remainder;
      if (i->opcode == AST_MACHINE_X86_SDIV) {
        int64_t sx = signed_value(x, ops[2].bit_width),
                sy = signed_value(y, ops[4].bit_width);
        if (sx == INT64_MIN && sy == -1)
          return 0;
        quotient = (uint64_t)(sx / sy);
        remainder = (uint64_t)(sx % sy);
      } else {
        quotient = x / y;
        remainder = x % y;
      }
      put_u64(out, quotient);
      if (!write_value(e, &ops[0], out))
        return 0;
      put_u64(out, remainder);
      if (!write_value(e, &ops[1], out))
        return 0;
      continue;
    }
    default:
      if (i->operand_count < 2u || !read_value(e, &ops[1], a))
        return 0;
      if (i->operand_count > 2u && !read_value(e, &ops[2], b))
        return 0;
      x = get_u64(a);
      y = get_u64(b);
      switch (i->opcode) {
      case AST_MACHINE_X86_ADD:
        value = x + y;
        break;
      case AST_MACHINE_X86_SUB:
        value = x - y;
        break;
      case AST_MACHINE_X86_MUL:
        value = x * y;
        break;
      case AST_MACHINE_X86_AND:
        value = x & y;
        break;
      case AST_MACHINE_X86_OR:
        value = x | y;
        break;
      case AST_MACHINE_X86_XOR:
        value = x ^ y;
        break;
      case AST_MACHINE_X86_NEG:
        value = UINT64_C(0) - x;
        break;
      case AST_MACHINE_X86_NOT:
        value = ~x;
        break;
      case AST_MACHINE_X86_SHL:
        value = x << (y & (ops[0].bit_width == 64u ? 63u : 31u));
        break;
      case AST_MACHINE_X86_LSHR:
        value = x >> (y & (ops[0].bit_width == 64u ? 63u : 31u));
        break;
      case AST_MACHINE_X86_ASHR:
        value = (uint64_t)(signed_value(x, ops[1].bit_width) >>
                           (y & (ops[0].bit_width == 64u ? 63u : 31u)));
        break;
      case AST_MACHINE_X86_ICMP_EQ:
        value = (uint64_t)(x == y);
        break;
      case AST_MACHINE_X86_ICMP_NE:
        value = (uint64_t)(x != y);
        break;
      case AST_MACHINE_X86_ICMP_LT:
        value = (uint64_t)(i->is_signed ? signed_value(x, ops[1].bit_width) <
                                              signed_value(y, ops[2].bit_width)
                                        : x < y);
        break;
      case AST_MACHINE_X86_ICMP_LE:
        value = (uint64_t)(i->is_signed ? signed_value(x, ops[1].bit_width) <=
                                              signed_value(y, ops[2].bit_width)
                                        : x <= y);
        break;
      case AST_MACHINE_X86_ICMP_GT:
        value = (uint64_t)(i->is_signed ? signed_value(x, ops[1].bit_width) >
                                              signed_value(y, ops[2].bit_width)
                                        : x > y);
        break;
      case AST_MACHINE_X86_ICMP_GE:
        value = (uint64_t)(i->is_signed ? signed_value(x, ops[1].bit_width) >=
                                              signed_value(y, ops[2].bit_width)
                                        : x >= y);
        break;
      case AST_MACHINE_X86_FADD:
      case AST_MACHINE_X86_FSUB:
      case AST_MACHINE_X86_FMUL:
      case AST_MACHINE_X86_FDIV: {
        if (ops[1].bit_width == 32u) {
          float fx, fy, fz;
          memcpy(&fx, a, 4u);
          memcpy(&fy, b, 4u);
          fz = i->opcode == AST_MACHINE_X86_FADD   ? fx + fy
               : i->opcode == AST_MACHINE_X86_FSUB ? fx - fy
               : i->opcode == AST_MACHINE_X86_FMUL ? fx * fy
                                                   : fx / fy;
          value = 0u;
          memcpy(&value, &fz, 4u);
          break;
        }
        double dx, dy, dz;
        memcpy(&dx, a, 8u);
        memcpy(&dy, b, 8u);
        dz = i->opcode == AST_MACHINE_X86_FADD   ? dx + dy
             : i->opcode == AST_MACHINE_X86_FSUB ? dx - dy
             : i->opcode == AST_MACHINE_X86_FMUL ? dx * dy
                                                 : dx / dy;
        memcpy(&value, &dz, 8u);
        break;
      }
      case AST_MACHINE_X86_CAST:
        value = x;
        break;
      case AST_MACHINE_X86_MOVSX:
        value = (uint64_t)signed_value(x, ops[1].bit_width);
        break;
      case AST_MACHINE_X86_MOVZX:
        value = x & width_mask(ops[1].bit_width);
        break;
      case AST_MACHINE_X86_POPCNT:
        for (uint32_t bit = 0; bit < ops[1].bit_width; ++bit)
          value += (x >> bit) & 1u;
        break;
      default:
        fprintf(stderr, "interpreter unsupported %s\n",
                ast_machine_opcode_name(i->opcode));
        return 0;
      }
      break;
    }
    value &= width_mask(ops[0].bit_width);
    put_u64(out, value);
    if (!write_value(e, &ops[0], out))
      return 0;
  }
}
static uint32_t register_unit(const Fixture *f, const char *name) {
  MachinePhysRegView view;
  return machine_physreg_view(&f->allocation->target, name, &view)
             ? view.alias_unit
             : AST_MACHINE_INVALID_ID;
}
static int execute_case(Fixture *f, uint32_t test, unsigned entry_variant) {
  static const char *names[] = {"add",
                                "subtract",
                                "multiply",
                                "bit_mix",
                                "divide",
                                "remainder",
                                "signed_divide",
                                "shift_left",
                                "shift_right",
                                "choose",
                                "loop_sum",
                                "call_live",
                                "sum9",
                                "call9",
                                "pair",
                                "triple",
                                "mixed",
                                "hfa",
                                "float_add",
                                "float_const",
                                "triple_call",
                                "pair_call",
                                "mixed_call",
                                "sum3",
                                "logical_not",
                                "logical_not",
                                "divide_i8",
                                "divide_i16",
                                "divide_u8",
                                "remainder_u16",
                                "remainder_i8",
                                "extend_signed",
                                "extend_unsigned",
                                "truncate16",
                                "shift_signed32",
                                "compare_signed16",
                                "float32_add",
                                "choose_false",
                                "divide_i8",
                                "min_constant",
                                "identity",
                                "arithmetic",
                                "multiple_outputs",
                                "hard_clobber",
                                "exact_bridge",
                                "byte_add",
                                "short_add",
                                "word_add",
                                "memory_read",
                                "memory_write",
                                "population",
                                "consecutive",
                                "raw",
                                "raw",
                                "forward32",
                                "forward64",
                                "call_live",
                                "call_live",
                                "choose",
                                "loop_sum"};
  uint32_t function = find_function(f, names[test]);
  if (test == 10u && f->allocation->config.register_limit == 0u) {
    int split = 0;
    for (uint32_t fi = 0; fi < f->allocation->function_count; ++fi)
      for (uint32_t v = 0;
           v < f->allocation->machine->functions[fi].info.vreg_count; ++v)
        if (f->allocation->functions[fi].location_piece_counts[v] > 1u) {
          const RegAllocLocationPiece *pieces =
              f->allocation->functions[fi].location_pieces[v];
          for (uint32_t s = 1;
               s < f->allocation->functions[fi].location_piece_counts[v]; ++s)
            if (pieces[s].location.kind != pieces[0].location.kind ||
                pieces[s].location.register_unit !=
                    pieces[0].location.register_unit ||
                pieces[s].location.frame_object !=
                    pieces[0].location.frame_object)
              split = 1;
        }
    if (!split) {
      fputs("loop_sum did not assign distinct homes to disjoint live-range "
            "pieces\n",
            stderr);
      return 0;
    }
  }
  if (function == AST_MACHINE_INVALID_ID)
    return 0;
  Execution *e = (Execution *)calloc(1u, sizeof(*e));
  if (!e)
    return 0;
  e->fixture = f;
  for (uint32_t u = 0; u < MACHINE_REGISTER_UNITS; ++u)
    memset(e->regs[u], (int)(u % 251u), sizeof(e->regs[u]));
  unsigned char preserved[MACHINE_REGISTER_UNITS][16];
  for (uint32_t u = 0; u < MACHINE_REGISTER_UNITS; ++u)
    memcpy(preserved[u], e->regs[u], 16u);
  const uint64_t entry_sp = test == 54u   ? 200024u + entry_variant * 32u
                            : test == 55u ? 200056u
                                          : 200008u + entry_variant * 16u;
  const uint64_t result_buffer = 65536u;
  put_u64(e->regs[MACHINE_PHYSREG_FILE_GPR * 64u + 4u], entry_sp);
  uint64_t arguments[9][8] = {{13u}, {5u},  {7u},  {11u}, {17u},
                              {19u}, {23u}, {29u}, {31u}};
  uint64_t expected[8] = {0};
  uint32_t expected_words = 1u;
  switch (test) {
  case 0:
    expected[0] = 18u;
    break;
  case 1:
    expected[0] = 8u;
    break;
  case 2:
    expected[0] = 65u;
    break;
  case 3:
    expected[0] = (13u ^ 5u) & 7u;
    break;
  case 4:
    expected[0] = 2u;
    break;
  case 5:
    expected[0] = 3u;
    break;
  case 6:
    arguments[0][0] = (uint64_t)-13;
    expected[0] = (uint64_t)-2;
    break;
  case 7:
    expected[0] = 416u;
    break;
  case 8:
    expected[0] = 0u;
    break;
  case 9:
    arguments[0][0] = 1u;
    expected[0] = 5u;
    break;
  case 10:
    expected[0] = 78u;
    break;
  case 11:
    expected[0] = 38u;
    break;
  case 12:
    expected[0] = 155u;
    break;
  case 13:
    expected[0] = 90u;
    break;
  case 14:
  case 15:
  case 16:
  case 17:
  case 20:
  case 21:
  case 22:
    arguments[0][0] = UINT64_C(0x0123456789abcdef);
    arguments[0][1] = UINT64_C(0x4022800000000000);
    arguments[0][2] = UINT64_C(0xfedcba9876543210);
    expected_words = test == 15u || test == 20u ? 3u : 2u;
    memcpy(expected, arguments[0], (size_t)expected_words * 8u);
    break;
  case 18:
  case 19: {
    double x = 1.25, y = 4.5, result = test == 18u ? 5.75 : 3.75;
    memcpy(&arguments[0][0], &x, 8u);
    memcpy(&arguments[1][0], &y, 8u);
    memcpy(&expected[0], &result, 8u);
    break;
  }
  case 23:
    expected[0] = 25u;
    break;
  case 24:
    arguments[0][0] = 0u;
    expected[0] = 1u;
    break;
  case 25:
    arguments[0][0] = 1u;
    expected[0] = 0u;
    break;
  case 26:
    arguments[0][0] = 155u;
    arguments[1][0] = 7u;
    expected[0] = 242u;
    break;
  case 27:
    arguments[0][0] = 35536u;
    arguments[1][0] = 7u;
    expected[0] = 61251u;
    break;
  case 28:
    arguments[0][0] = 255u;
    arguments[1][0] = 19u;
    expected[0] = 13u;
    break;
  case 29:
    arguments[0][0] = 65535u;
    arguments[1][0] = 257u;
    expected[0] = 0u;
    break;
  case 30:
    arguments[0][0] = 155u;
    arguments[1][0] = 7u;
    expected[0] = 253u;
    break;
  case 31:
    arguments[0][0] = 155u;
    expected[0] = (uint64_t)-101;
    break;
  case 32:
    arguments[0][0] = UINT32_MAX;
    expected[0] = UINT32_MAX;
    break;
  case 33:
    arguments[0][0] = UINT64_C(0xfeed1234);
    expected[0] = 0x1234u;
    break;
  case 34:
    arguments[0][0] = (uint32_t)-80;
    arguments[1][0] = 2u;
    expected[0] = (uint32_t)-20;
    break;
  case 35:
    arguments[0][0] = (uint16_t)-5;
    arguments[1][0] = 3u;
    expected[0] = 1u;
    break;
  case 36: {
    float x = 1.25f, y = 2.5f, result = 3.75f;
    memcpy(&arguments[0][0], &x, 4u);
    memcpy(&arguments[1][0], &y, 4u);
    memcpy(&expected[0], &result, 4u);
    break;
  }
  case 37:
    arguments[0][0] = 0u;
    expected[0] = 7u;
    break;
  case 38:
    arguments[0][0] = 128u;
    arguments[1][0] = 255u;
    expected[0] = 128u;
    break;
  case 39:
    expected[0] = UINT64_C(0x8000000000000000);
    break;
  case 40:
    expected[0] = 13u;
    break;
  case 41:
    expected[0] = ((((13u + 5u - 3u) * 5u) & 255u) ^ 5u) | 1u;
    break;
  case 42:
    expected[0] = (13u + 5u) * 17u + 4u;
    break;
  case 43:
    expected[0] = 13u + 7u + 13u;
    break;
  case 44:
    expected[0] = 13u + 5u + 13u;
    break;
  case 45:
    arguments[0][0] = 255u;
    expected[0] = 0u;
    break;
  case 46:
    arguments[0][0] = 65535u;
    expected[0] = 0u;
    break;
  case 47:
    arguments[0][0] = UINT32_MAX;
    expected[0] = 0u;
    break;
  case 48:
    arguments[0][0] = 1024u;
    put_u64(e->memory + 1032u, 123456u);
    expected[0] = 123456u;
    break;
  case 49:
    arguments[0][0] = 1024u;
    expected[0] = 6u;
    break;
  case 50:
    arguments[0][0] = UINT64_C(0xf0f0000000000001);
    expected[0] = 9u;
    break;
  case 51:
    expected[0] = (13u + 7u) * 13u;
    break;
  case 52:
    expected[0] = 0u;
    break;
  case 53:
    expected[0] = 7u;
    break;
  case 54:
  case 55:
    expected_words = test == 54u ? 4u : 8u;
    for (uint32_t w = 0; w < expected_words; ++w)
      expected[w] = arguments[0][w] = UINT64_C(0xf123456789abcdef) + w;
    break;
  case 56:
  case 57:
    expected[0] = 38u;
    break;
  case 58:
    arguments[0][0] = entry_variant & 1u;
    expected[0] = arguments[0][0] ? 5u : 7u;
    break;
  case 59:
    expected[0] = 78u;
    break;
  default:
    free(e);
    return 0;
  }
  AstAbiFunctionInfo abi_fn;
  if (!ast_abi_function_info(f->abi, function, &abi_fn)) {
    free(e);
    return 0;
  }
  uint32_t user_arg = 0u;
  for (uint32_t a = 0; a < abi_fn.argument_count + abi_fn.hidden_argument_count;
       ++a) {
    AstAbiValueInfo v;
    if (!ast_abi_function_argument(f->abi, function, a, &v)) {
      free(e);
      return 0;
    }
    uint64_t pointer = 0u;
    const unsigned char *bytes;
    uint64_t size;
    if (v.flags & AST_ABI_VALUE_HIDDEN) {
      pointer = result_buffer;
      bytes = (const unsigned char *)&pointer;
      size = 8u;
    } else {
      size = ast_sema_type_size(v.type);
      bytes = (const unsigned char *)arguments[user_arg++];
      if (v.location.kind == AST_ABI_LOCATION_INDIRECT) {
        pointer = 70000u + (uint64_t)a * 64u;
        memcpy(e->memory + pointer, bytes, (size_t)size);
        bytes = (const unsigned char *)&pointer;
        size = 8u;
      }
    }
    uint64_t offset = 0u;
    for (uint32_t r = 0; r < v.location.register_count && offset < size; ++r) {
      uint64_t piece = size - offset;
      if (!(v.location.register_count == 1u &&
            v.location.registers[0][0] == 'x') &&
          piece > 8u)
        piece = 8u;
      uint32_t unit = register_unit(f, v.location.registers[r]);
      if (unit == AST_MACHINE_INVALID_ID) {
        free(e);
        return 0;
      }
      memcpy(e->regs[unit], bytes + offset, (size_t)piece);
      offset += piece;
    }
    if (offset < size)
      memcpy(e->memory + entry_sp + 8u + v.location.stack_offset,
             bytes + offset, (size_t)(size - offset));
  }
  int ok = execute_function(e, function) &&
           get_u64(e->regs[MACHINE_PHYSREG_FILE_GPR * 64u + 4u]) == entry_sp;
  if (test == 49u && get_u64(e->memory + 1024u) != 5u)
    ok = 0;
  if (test == 52u && get_u64(e->regs[register_unit(f, "rax")]) != 7u)
    ok = 0;
  if (test >= 52u && test < 54u &&
      (f->allocation->functions[function].object_count ||
       f->allocation->functions[function].frame.stack_adjustment))
    ok = 0;
  if (test >= 54u &&
      (!f->allocation->functions[function].frame.realigns_stack ||
       f->allocation->functions[function].frame.alignment !=
           (test == 54u || test == 56u ? 32u : 64u)))
    ok = 0;
  unsigned char result[64] = {0};
  if (abi_fn.result.location.kind == AST_ABI_LOCATION_INDIRECT) {
    memcpy(result, e->memory + result_buffer, (size_t)expected_words * 8u);
    if (get_u64(e->regs[register_unit(f, "rax")]) != result_buffer)
      ok = 0;
  } else {
    uint64_t size = ast_sema_type_size(abi_fn.result.type), offset = 0u;
    for (uint32_t r = 0;
         r < abi_fn.result.location.register_count && offset < size; ++r) {
      uint64_t bytes = size - offset;
      if (!(abi_fn.result.location.register_count == 1u &&
            abi_fn.result.location.registers[0][0] == 'x') &&
          bytes > 8u)
        bytes = 8u;
      uint32_t unit = register_unit(f, abi_fn.result.location.registers[r]);
      if (unit == AST_MACHINE_INVALID_ID || unit >= MACHINE_REGISTER_UNITS) {
        free(e);
        return 0;
      }
      memcpy(result + offset, e->regs[unit], (size_t)bytes);
      offset += bytes;
    }
  }
  if (memcmp(result, expected, (size_t)expected_words * 8u)) {
    fprintf(stderr, "%s: got %llx, expected %llx\n", names[test],
            (unsigned long long)get_u64(result),
            (unsigned long long)expected[0]);
    ok = 0;
  }
  for (uint32_t unit = 0; unit < MACHINE_REGISTER_UNITS; ++unit) {
    MachinePhysRegView view = {0};
    view.file = (MachinePhysRegFile)(unit / 64u);
    view.index = unit % 64u;
    uint32_t bytes =
        machine_physreg_preserved_bits(f->allocation->abi, &view) / 8u;
    if (bytes && memcmp(preserved[unit], e->regs[unit], bytes)) {
      fprintf(stderr, "callee-save corruption: unit %u\n", unit);
      ok = 0;
    }
  }
  if (!ok)
    ast_regalloc_dump(f->allocation, print_dump, stderr);
  free(e);
  return ok;
}
static int check_realignment_contract(Fixture *f) {
  for (uint32_t fn = 0; fn < f->allocation->function_count; ++fn) {
    AllocationFunction *af = &f->allocation->functions[fn];
    AstEncoderOutput encoded;
    if (!ast_encoder_x86_64_function(f->allocation, fn, &encoded)) {
      fprintf(stderr, "encoder integration: %s\n", encoded.message);
      ast_encoder_output_destroy(&encoded);
      return 0;
    }
    if (!encoded.size) {
      ast_encoder_output_destroy(&encoded);
      return 0;
    }
    ast_encoder_output_destroy(&encoded);
    if (!af->frame.realigns_stack)
      continue;
    AstFrameInfo frame = af->frame;
    af->frame.alignment = 16u;
    int rejected = !ast_physical_verify(f->allocation);
    af->frame = frame;
    if (!rejected) {
      fputs("verifier accepted lost frame alignment\n", stderr);
      return 0;
    }
    af->frame.realigns_stack = 0u;
    rejected = !ast_physical_verify(f->allocation);
    af->frame = frame;
    if (!rejected) {
      fputs("verifier accepted missing realignment flag\n", stderr);
      return 0;
    }
    PhysicalBlock *entry =
        &af->blocks[f->machine->functions[fn].info.entry_block];
    AstPhysicalInstructionInfo saved = entry->instructions[3];
    entry->instructions[3].operands[2].immediate_bits = (uint64_t)-16;
    rejected = !ast_physical_verify(f->allocation);
    entry->instructions[3] = saved;
    if (!rejected) {
      fputs("verifier accepted wrong AND mask\n", stderr);
      return 0;
    }
    saved = entry->instructions[2];
    entry->instructions[2].opcode = AST_MACHINE_X86_MOV_IMM;
    rejected = !ast_physical_verify(f->allocation);
    entry->instructions[2] = saved;
    if (!rejected) {
      fputs("verifier accepted missing incoming base\n", stderr);
      return 0;
    }
    for (uint32_t b = 0; b < af->block_count; ++b) {
      PhysicalBlock *bb = &af->blocks[b];
      if (bb->instructions[bb->count - 1u].opcode != AST_MACHINE_X86_RET)
        continue;
      saved = bb->instructions[bb->count - 3u];
      bb->instructions[bb->count - 3u].operands[1].displacement = 8;
      rejected = !ast_physical_verify(f->allocation);
      bb->instructions[bb->count - 3u] = saved;
      if (!rejected) {
        fputs("verifier accepted wrong saved RBP slot\n", stderr);
        return 0;
      }
    }
  }
  return ast_physical_verify(f->allocation);
}
int main(int argc, char **argv) {
  ast_test_process_setup();
  if (argc < 4 || argc > 6)
    return 2;
  char *end = NULL;
  unsigned long test = strtoul(argv[1], &end, 10);
  if (!end || *end || test >= 60u)
    return 2;
  int windows = strcmp(argv[2], "win64") == 0;
  uint32_t limit = (uint32_t)strtoul(argv[3], &end, 10);
  if (!end || *end || limit > 16u)
    return 2;
  Fixture f;
  if (!open_fixture(&f, windows, limit,
                    argc >= 5 ? strcmp(argv[4], "0") != 0 : 1, (uint32_t)test,
                    argc == 6 ? (uint32_t)strtoul(argv[5], NULL, 10) : 0u))
    return 1;
  int ok = 1;
  unsigned variants = test >= 56u ? 4u : test == 54u ? 2u : 1u;
  for (unsigned v = 0; ok && v < variants; ++v)
    ok = execute_case(&f, (uint32_t)test, v);
  if (ok && test >= 54u)
    ok = check_realignment_contract(&f);
  close_fixture(&f);
  return ok ? 0 : 1;
}
