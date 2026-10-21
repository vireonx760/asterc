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
static int open_fixture(Fixture *f, uint32_t limit, int promote, AstAbiKind abi,
                        uint32_t frame_bytes, uint32_t test) {
  char root[4096], entry[4096];
  memset(f, 0, sizeof(*f));
  (void)snprintf(root, sizeof(root), "%s/tests/regalloc_cases/%s",
                 ASTERASM_SOURCE_DIR,
                 test >= 40u   ? "crossarch_scalar"
                 : test >= 24u ? "x86_scalar"
                               : "core");
  (void)snprintf(entry, sizeof(entry), "%s/tests/regalloc_cases/%s/main.aa",
                 ASTERASM_SOURCE_DIR,
                 test >= 40u   ? "crossarch_scalar"
                 : test >= 24u ? "x86_scalar"
                               : "core");
  AstFrontendConfig fc = ast_frontend_config_default();
  fc.thread_count = 1u;
  f->frontend = ast_frontend_create(&fc);
  if (!f->frontend || ast_frontend_add_import_root(f->frontend, root) != 0)
    goto fail;
  const char *entries[] = {entry};
  if (ast_frontend_load(f->frontend, entries, 1u) != 0) {
    fputs("frontend failed\n", stderr);
    goto fail;
  }
  AstSemaConfig sc = ast_sema_config_default();
  sc.thread_count = 1u;
  f->sema = ast_sema_create(f->frontend, &sc);
  if (!f->sema || ast_sema_check(f->sema) != 0) {
    fputs("sema failed\n", stderr);
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
  AstTargetConfig tc = ast_target_config_default(AST_TARGET_ARCH_RISCV64);
  tc.os = AST_TARGET_OS_LINUX;
  tc.features |= AST_TARGET_FEATURE_RV_M | AST_TARGET_FEATURE_RV_F |
                 AST_TARGET_FEATURE_RV_D;
  f->target = ast_target_legalize(f->mir, &tc);
  if (!f->target || !ast_target_verify(f->target)) {
    fputs("target failed\n", stderr);
    goto fail;
  }
  AstAbiConfig ac = ast_abi_config_default(abi);
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
   aliases, byte-addressed memory, architectural NZCV branch state and
   link-register calls, independently of the allocator's home/interval
   decisions. It never reads virtual registers. */
#define TEST_MEMORY_SIZE 262144u
typedef struct Execution {
  unsigned char regs[MACHINE_REGISTER_UNITS][64];
  unsigned char memory[TEST_MEMORY_SIZE];
  const Fixture *fixture;
  uint32_t steps;
  uint32_t depth;
  int zero_flag;
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
    if (o->bank == AST_MACHINE_BANK_GPR)
      bytes = 8u;
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
    if (o->bank == AST_MACHINE_BANK_GPR) {
      if (o->register_unit == MACHINE_PHYSREG_FILE_GPR * 64u)
        return 1;
      bytes = 8u;
    }
    if (o->bank == AST_MACHINE_BANK_FP && o->bit_width == 32u)
      memset(e->regs[o->register_unit] + 4u, 0xff, 4u);
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
static double floating_value(Execution *e, const AstPhysicalOperand *op,
                             const unsigned char value[64]) {
  if (op->bit_width == 32u) {
    uint32_t high;
    memcpy(&high, e->regs[op->register_unit] + 4u, 4u);
    uint32_t low;
    memcpy(&low, value, 4u);
    if (high != UINT32_MAX)
      low = UINT32_C(0x7fc00000);
    float f;
    memcpy(&f, &low, 4u);
    return f;
  }
  double d;
  memcpy(&d, value, 8u);
  return d;
}
static int execute_function(Execution *e, uint32_t function) {
  if (function >= e->fixture->allocation->function_count || ++e->depth > 32u)
    return 0;
  const AllocationFunction *fn = &e->fixture->allocation->functions[function];
  uint32_t block = e->fixture->machine->functions[function].info.entry_block,
           index = 0u;
  const uint32_t sp_unit = (MACHINE_PHYSREG_FILE_GPR * 64u + 2u);
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
    case AST_MACHINE_RISCV_MV:
    case AST_MACHINE_RISCV_LI:
    case AST_MACHINE_RISCV_LBU:
    case AST_MACHINE_RISCV_LHU:
    case AST_MACHINE_RISCV_LWU:
    case AST_MACHINE_RISCV_LD:
    case AST_MACHINE_RISCV_FLW:
    case AST_MACHINE_RISCV_FLD:
    case AST_MACHINE_RISCV_FMV_S:
    case AST_MACHINE_RISCV_FMV_D:
    case AST_MACHINE_RISCV_FMV_W_X:
    case AST_MACHINE_RISCV_FMV_D_X:
    case AST_MACHINE_RISCV_FMV_X_W:
    case AST_MACHINE_RISCV_FMV_X_D:
      if (!read_value(e, &ops[1], out) || !write_value(e, &ops[0], out))
        return 0;
      continue;
    case AST_MACHINE_RISCV_SB:
    case AST_MACHINE_RISCV_SH:
    case AST_MACHINE_RISCV_SW:
    case AST_MACHINE_RISCV_SD:
    case AST_MACHINE_RISCV_FSW:
    case AST_MACHINE_RISCV_FSD:
      if (!read_value(e, &ops[0], out) || !write_value(e, &ops[1], out))
        return 0;
      continue;
    case AST_MACHINE_RISCV_LA:
      if (ops[1].kind != AST_PHYSICAL_OPERAND_MEMORY)
        return 0;
      put_u64(out, get_u64(e->regs[ops[1].register_unit]) +
                       (uint64_t)ops[1].displacement);
      if (!write_value(e, &ops[0], out))
        return 0;
      continue;
    case AST_MACHINE_RISCV_J:
      block = ops[0].block;
      index = 0u;
      continue;
    case AST_MACHINE_RISCV_CMP_BR: {
      if (ops[0].kind != AST_PHYSICAL_OPERAND_IMMEDIATE ||
          !read_value(e, &ops[1], a) || !read_value(e, &ops[2], b) ||
          ops[3].kind != AST_PHYSICAL_OPERAND_BLOCK ||
          ops[4].kind != AST_PHYSICAL_OPERAND_BLOCK)
        return 0;
      x = get_u64(a);
      y = get_u64(b);
      int take = 0;
      switch ((unsigned)ops[0].immediate_bits) {
      case 0u:
        take = x == y;
        break;
      case 1u:
        take = x != y;
        break;
      case 4u:
        take = (int64_t)x < (int64_t)y;
        break;
      case 5u:
        take = (int64_t)x >= (int64_t)y;
        break;
      case 6u:
        take = x < y;
        break;
      case 7u:
        take = x >= y;
        break;
      default:
        return 0;
      }
      block = ops[take ? 3u : 4u].block;
      index = 0u;
      continue;
    }
    case AST_MACHINE_RISCV_BNE:
      if (!read_value(e, &ops[0], a) || !read_value(e, &ops[1], b))
        return 0;
      if (get_u64(a) != get_u64(b)) {
        block = ops[2].block;
        index = 0u;
      }
      continue;
    case AST_MACHINE_RISCV_RET:
      --e->depth;
      return 1;
    case AST_MACHINE_RISCV_EBREAK:
      return 0;
    case AST_MACHINE_RISCV_CALL: {
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
      uint64_t return_address = UINT64_C(0xaa640000) + e->steps;
      put_u64(e->regs[MACHINE_PHYSREG_FILE_GPR * 64u + 1u], return_address);
      if ((sp & 15u) || !execute_function(e, target) ||
          get_u64(e->regs[sp_unit]) != sp ||
          get_u64(e->regs[MACHINE_PHYSREG_FILE_GPR * 64u + 1u]) !=
              return_address)
        return 0;
      /* An ABI-compliant callee may destroy all volatile registers.
         Preserve only result locations and nonvolatile widths. */
      for (uint32_t unit = 0; unit < MACHINE_REGISTER_UNITS; ++unit) {
        uint32_t file = unit / 64u, reg = unit % 64u;
        if (unit == sp_unit ||
            (file == MACHINE_PHYSREG_FILE_GPR &&
             (reg <= 4u || reg == 10u || reg == 11u)) ||
            (file == MACHINE_PHYSREG_FILE_FP_VECTOR &&
             (reg == 10u || reg == 11u)))
          continue;
        MachinePhysRegView view = {0};
        view.file = (MachinePhysRegFile)file;
        view.index = reg;
        uint32_t preserved =
            machine_physreg_preserved_bits(e->fixture->allocation->abi, &view) /
            8u;
        /* LP64F preserves an f32 value, including the NaN-boxing
           needed to read it on an FLEN=64 machine. A real FLW
           restore fills the upper 32 bits with ones. Arbitrarily
           poisoning those bits would not be ABI-compliant. */
        if (file == MACHINE_PHYSREG_FILE_FP_VECTOR && preserved == 4u) {
          memset(e->regs[unit] + 4u, 0xff, 4u);
          preserved = 8u;
        }
        if (preserved < 64u)
          memset(e->regs[unit] + preserved, 0xa5, 64u - preserved);
      }
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
      case AST_MACHINE_RISCV_ADD:
      case AST_MACHINE_RISCV_ADDI:
        value = x + y;
        break;
      case AST_MACHINE_RISCV_SUB:
        value = x - y;
        break;
      case AST_MACHINE_RISCV_MUL:
        value = x * y;
        break;
      case AST_MACHINE_RISCV_DIVU:
        value = y ? x / y : UINT64_MAX;
        break;
      case AST_MACHINE_RISCV_REMU:
        value = y ? x % y : x;
        break;
      case AST_MACHINE_RISCV_DIV:
        value = !y ? UINT64_MAX
                : x == (uint64_t)INT64_MIN && y == UINT64_MAX
                    ? x
                    : (uint64_t)((int64_t)x / (int64_t)y);
        break;
      case AST_MACHINE_RISCV_REM:
        value = !y ? x
                : x == (uint64_t)INT64_MIN && y == UINT64_MAX
                    ? 0u
                    : (uint64_t)((int64_t)x % (int64_t)y);
        break;
      case AST_MACHINE_RISCV_AND:
        value = x & y;
        break;
      case AST_MACHINE_RISCV_OR:
        value = x | y;
        break;
      case AST_MACHINE_RISCV_XOR:
      case AST_MACHINE_RISCV_XORI:
        value = x ^ y;
        break;
      case AST_MACHINE_RISCV_NEG:
        value = UINT64_C(0) - x;
        break;
      case AST_MACHINE_RISCV_NOT:
        value = ~x;
        break;
      case AST_MACHINE_RISCV_SLL:
      case AST_MACHINE_RISCV_SLLI:
        value = x << (y & 63u);
        break;
      case AST_MACHINE_RISCV_SRL:
      case AST_MACHINE_RISCV_SRLI:
        value = x >> (y & 63u);
        break;
      case AST_MACHINE_RISCV_SRA:
      case AST_MACHINE_RISCV_SRAI:
        value = (uint64_t)((int64_t)x >> (y & 63u));
        break;
      case AST_MACHINE_RISCV_SLT:
        value = (uint64_t)((int64_t)x < (int64_t)y);
        break;
      case AST_MACHINE_RISCV_SLTU:
      case AST_MACHINE_RISCV_SLTIU:
        value = (uint64_t)(x < y);
        break;
      case AST_MACHINE_RISCV_ADDW:
      case AST_MACHINE_RISCV_ADDIW:
        value = (uint64_t)(int64_t)(int32_t)(uint32_t)(x + y);
        break;
      case AST_MACHINE_RISCV_SUBW:
        value = (uint64_t)(int64_t)(int32_t)(uint32_t)(x - y);
        break;
      case AST_MACHINE_RISCV_MULW:
        value = (uint64_t)(int64_t)(int32_t)(uint32_t)(x * y);
        break;
      case AST_MACHINE_RISCV_SLLW:
        value = (uint64_t)(int64_t)(int32_t)((uint32_t)x << (y & 31u));
        break;
      case AST_MACHINE_RISCV_SRLW:
        value = (uint64_t)(int64_t)(int32_t)((uint32_t)x >> (y & 31u));
        break;
      case AST_MACHINE_RISCV_SRAW:
        value = (uint64_t)(int64_t)((int32_t)(uint32_t)x >> (y & 31u));
        break;
      case AST_MACHINE_RISCV_DIVW:
      case AST_MACHINE_RISCV_REMW: {
        int32_t sx = (int32_t)(uint32_t)x, sy = (int32_t)(uint32_t)y;
        int32_t d = !sy ? -1 : sx == INT32_MIN && sy == -1 ? sx : sx / sy;
        int32_t r = !sy ? sx : sx == INT32_MIN && sy == -1 ? 0 : sx % sy;
        value =
            (uint64_t)(int64_t)(i->opcode == AST_MACHINE_RISCV_DIVW ? d : r);
        break;
      }
      case AST_MACHINE_RISCV_DIVUW:
      case AST_MACHINE_RISCV_REMUW: {
        uint32_t ux = (uint32_t)x, uy = (uint32_t)y;
        uint32_t d = uy ? ux / uy : UINT32_MAX, r = uy ? ux % uy : ux;
        value =
            (uint64_t)(int64_t)(int32_t)(i->opcode == AST_MACHINE_RISCV_DIVUW
                                             ? d
                                             : r);
        break;
      }
      case AST_MACHINE_RISCV_FADD_S:
      case AST_MACHINE_RISCV_FSUB_S:
      case AST_MACHINE_RISCV_FMUL_S:
      case AST_MACHINE_RISCV_FDIV_S: {
        float fx = (float)floating_value(e, &ops[1], a),
              fy = (float)floating_value(e, &ops[2], b), fz;
        fz = i->opcode == AST_MACHINE_RISCV_FADD_S   ? fx + fy
             : i->opcode == AST_MACHINE_RISCV_FSUB_S ? fx - fy
             : i->opcode == AST_MACHINE_RISCV_FMUL_S ? fx * fy
                                                     : fx / fy;
        memcpy(&value, &fz, 4u);
        break;
      }
      case AST_MACHINE_RISCV_FADD_D:
      case AST_MACHINE_RISCV_FSUB_D:
      case AST_MACHINE_RISCV_FMUL_D:
      case AST_MACHINE_RISCV_FDIV_D: {
        double dx, dy, dz;
        memcpy(&dx, a, 8u);
        memcpy(&dy, b, 8u);
        dz = i->opcode == AST_MACHINE_RISCV_FADD_D   ? dx + dy
             : i->opcode == AST_MACHINE_RISCV_FSUB_D ? dx - dy
             : i->opcode == AST_MACHINE_RISCV_FMUL_D ? dx * dy
                                                     : dx / dy;
        memcpy(&value, &dz, 8u);
        break;
      }
      case AST_MACHINE_RISCV_FEQ_S:
      case AST_MACHINE_RISCV_FEQ_D:
      case AST_MACHINE_RISCV_FLT_S:
      case AST_MACHINE_RISCV_FLT_D:
      case AST_MACHINE_RISCV_FLE_S:
      case AST_MACHINE_RISCV_FLE_D: {
        double dx = floating_value(e, &ops[1], a),
               dy = floating_value(e, &ops[2], b);
        value = (uint64_t)((i->opcode == AST_MACHINE_RISCV_FEQ_S ||
                            i->opcode == AST_MACHINE_RISCV_FEQ_D)
                               ? dx == dy
                           : (i->opcode == AST_MACHINE_RISCV_FLT_S ||
                              i->opcode == AST_MACHINE_RISCV_FLT_D)
                               ? dx < dy
                               : dx <= dy);
        break;
      }
      case AST_MACHINE_RISCV_FCVT_S_W: {
        double converted = (double)(int32_t)(uint32_t)x;
        float f = (float)converted;
        memcpy(&value, &f, 4u);
        break;
      }
      case AST_MACHINE_RISCV_FCVT_W_S: {
        double converted = floating_value(e, &ops[1], a);
        value = (uint64_t)(int64_t)(int32_t)converted;
        break;
      }
      case AST_MACHINE_RISCV_FCVT_S_WU: {
        double converted = (double)(uint32_t)x;
        float f = (float)converted;
        memcpy(&value, &f, 4u);
        break;
      }
      case AST_MACHINE_RISCV_FCVT_WU_S: {
        double converted = floating_value(e, &ops[1], a);
        value = (uint64_t)(int64_t)(int32_t)(uint32_t)converted;
        break;
      }
      case AST_MACHINE_RISCV_FCVT_S_L: {
        double converted = (double)(int64_t)x;
        float f = (float)converted;
        memcpy(&value, &f, 4u);
        break;
      }
      case AST_MACHINE_RISCV_FCVT_L_S: {
        double converted = floating_value(e, &ops[1], a);
        value = (uint64_t)(int64_t)converted;
        break;
      }
      case AST_MACHINE_RISCV_FCVT_S_LU: {
        double converted = (double)x;
        float f = (float)converted;
        memcpy(&value, &f, 4u);
        break;
      }
      case AST_MACHINE_RISCV_FCVT_LU_S: {
        double converted = floating_value(e, &ops[1], a);
        value = (uint64_t)converted;
        break;
      }
      case AST_MACHINE_RISCV_FCVT_D_W: {
        double converted = (double)(int32_t)(uint32_t)x;
        memcpy(&value, &converted, 8u);
        break;
      }
      case AST_MACHINE_RISCV_FCVT_W_D: {
        double converted = floating_value(e, &ops[1], a);
        value = (uint64_t)(int64_t)(int32_t)converted;
        break;
      }
      case AST_MACHINE_RISCV_FCVT_D_WU: {
        double converted = (double)(uint32_t)x;
        memcpy(&value, &converted, 8u);
        break;
      }
      case AST_MACHINE_RISCV_FCVT_WU_D: {
        double converted = floating_value(e, &ops[1], a);
        value = (uint64_t)(int64_t)(int32_t)(uint32_t)converted;
        break;
      }
      case AST_MACHINE_RISCV_FCVT_D_L: {
        double converted = (double)(int64_t)x;
        memcpy(&value, &converted, 8u);
        break;
      }
      case AST_MACHINE_RISCV_FCVT_L_D: {
        double converted = floating_value(e, &ops[1], a);
        value = (uint64_t)(int64_t)converted;
        break;
      }
      case AST_MACHINE_RISCV_FCVT_D_LU: {
        double converted = (double)x;
        memcpy(&value, &converted, 8u);
        break;
      }
      case AST_MACHINE_RISCV_FCVT_LU_D: {
        double converted = floating_value(e, &ops[1], a);
        value = (uint64_t)converted;
        break;
      }
      case AST_MACHINE_RISCV_FCVT_S_D:
      case AST_MACHINE_RISCV_FCVT_D_S: {
        double converted = floating_value(e, &ops[1], a);
        if (ops[0].bit_width == 32u) {
          float f = (float)converted;
          memcpy(&value, &f, 4u);
        } else
          memcpy(&value, &converted, 8u);
        break;
      }
      default:
        fprintf(stderr, "interpreter unsupported %s\n",
                ast_machine_opcode_name(i->opcode));
        return 0;
      }
      break;
    }
    if (ops[0].bank != AST_MACHINE_BANK_GPR)
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
static int execute_case(Fixture *f, uint32_t test) {
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
                                "signed_to_float",
                                "unsigned_to_double",
                                "double_to_signed",
                                "double_to_unsigned",
                                "widen_float",
                                "narrow_float",
                                "bits_to_float",
                                "double_to_bits",
                                "float_less",
                                "float_not_equal",
                                "float_greater_equal",
                                "select_integer",
                                "select_integer",
                                "select_float",
                                "select_float",
                                "float_less_equal",
                                "float_equal",
                                "float_call_live"};
  uint32_t function = find_function(f, names[test]);
  if (function == AST_MACHINE_INVALID_ID)
    return 0;
  Execution *e = (Execution *)calloc(1u, sizeof(*e));
  if (!e)
    return 0;
  e->fixture = f;
  for (uint32_t u = 0; u < MACHINE_REGISTER_UNITS; ++u)
    memset(e->regs[u], (int)(u % 251u), sizeof(e->regs[u]));
  memset(e->regs[MACHINE_PHYSREG_FILE_GPR * 64u], 0, 64u);
  unsigned char preserved[MACHINE_REGISTER_UNITS][16];
  for (uint32_t u = 0; u < MACHINE_REGISTER_UNITS; ++u)
    memcpy(preserved[u], e->regs[u], 16u);
  const uint64_t entry_sp = 200000u, result_buffer = 65536u;
  put_u64(e->regs[(MACHINE_PHYSREG_FILE_GPR * 64u + 2u)], entry_sp);
  uint64_t arguments[9][4] = {{13u}, {5u},  {7u},  {11u}, {17u},
                              {19u}, {23u}, {29u}, {31u}};
  uint64_t expected[4] = {0};
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
  case 40: {
    float x = -101.0f;
    arguments[0][0] = 155u;
    memcpy(expected, &x, 4u);
    break;
  }
  case 41: {
    double x = 9223372036854777856.0;
    arguments[0][0] = UINT64_C(0x8000000000000800);
    memcpy(expected, &x, 8u);
    break;
  }
  case 42: {
    double x = -123.875;
    memcpy(arguments[0], &x, 8u);
    expected[0] = (uint64_t)-123;
    break;
  }
  case 43: {
    double x = 9223372036854775808.0;
    memcpy(arguments[0], &x, 8u);
    expected[0] = UINT64_C(0x8000000000000000);
    break;
  }
  case 44: {
    float x = 1.25f;
    double y = 1.25;
    memcpy(arguments[0], &x, 4u);
    memcpy(expected, &y, 8u);
    break;
  }
  case 45: {
    double x = 1.1;
    float y = 1.1f;
    memcpy(arguments[0], &x, 8u);
    memcpy(expected, &y, 4u);
    break;
  }
  case 46:
    arguments[0][0] = UINT32_C(0x80000000);
    expected[0] = UINT32_C(0x80000000);
    break;
  case 47:
    arguments[0][0] = UINT64_C(0x7ff8000000000042);
    expected[0] = arguments[0][0];
    break;
  case 48:
  case 49:
    arguments[0][0] = UINT64_C(0x7ff8000000000042);
    arguments[1][0] = 0u;
    expected[0] = test == 49u;
    break;
  case 50:
    arguments[0][0] = UINT32_C(0x7fc00042);
    arguments[1][0] = 0u;
    expected[0] = 0u;
    break;
  case 51:
  case 52:
    arguments[0][0] = test == 52u;
    expected[0] = test == 52u ? 5u : 7u;
    break;
  case 53:
  case 54: {
    float x = -0.0f, y = 3.25f;
    arguments[0][0] = test == 54u;
    memcpy(arguments[1], &x, 4u);
    memcpy(arguments[2], &y, 4u);
    memcpy(expected, test == 54u ? &x : &y, 4u);
    break;
  }
  case 55: {
    double x = -12.5, y = 3.75;
    memcpy(arguments[0], &x, 8u);
    memcpy(arguments[1], &y, 8u);
    expected[0] = 1u;
    break;
  }
  case 56:
    arguments[0][0] = UINT32_C(0x80000000);
    arguments[1][0] = 0u;
    expected[0] = 1u;
    break;
  case 57: {
    float x = 1.25f, y = 2.5f, live = 9.5f, result = 13.25f;
    memcpy(arguments[0], &x, 4u);
    memcpy(arguments[1], &y, 4u);
    memcpy(arguments[2], &live, 4u);
    memcpy(expected, &result, 4u);
    break;
  }
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
            v.location.registers[0][0] == 'z') &&
          piece > 8u)
        piece = 8u;
      uint32_t unit = register_unit(f, v.location.registers[r]);
      if (unit == AST_MACHINE_INVALID_ID) {
        free(e);
        return 0;
      }
      memcpy(e->regs[unit], bytes + offset, (size_t)piece);
      if (unit / 64u == MACHINE_PHYSREG_FILE_FP_VECTOR && piece == 4u)
        memset(e->regs[unit] + 4u, 0xff, 4u);
      offset += piece;
    }
    if (offset < size)
      memcpy(e->memory + entry_sp + v.location.stack_offset, bytes + offset,
             (size_t)(size - offset));
  }
  int ok = execute_function(e, function) &&
           get_u64(e->regs[(MACHINE_PHYSREG_FILE_GPR * 64u + 2u)]) == entry_sp;
  unsigned char result[32] = {0};
  if (abi_fn.result.location.kind == AST_ABI_LOCATION_INDIRECT) {
    memcpy(result, e->memory + result_buffer, (size_t)expected_words * 8u);

  } else {
    uint64_t size = ast_sema_type_size(abi_fn.result.type), offset = 0u;
    for (uint32_t r = 0;
         r < abi_fn.result.location.register_count && offset < size; ++r) {
      uint64_t bytes = size - offset;
      if (!(abi_fn.result.location.register_count == 1u &&
            abi_fn.result.location.registers[0][0] == 'z') &&
          bytes > 8u)
        bytes = 8u;
      memcpy(result + offset,
             e->regs[register_unit(f, abi_fn.result.location.registers[r])],
             (size_t)bytes);
      offset += bytes;
    }
  }
  if (memcmp(result, expected, (size_t)expected_words * 8u)) {
    fprintf(stderr, "%s: got %llx, expected %llx\n", names[test],
            (unsigned long long)get_u64(result),
            (unsigned long long)expected[0]);
    ok = 0;
  }
  if (memcmp(preserved[MACHINE_PHYSREG_FILE_GPR * 64u + 1u],
             e->regs[MACHINE_PHYSREG_FILE_GPR * 64u + 1u], 8u))
    ok = 0;
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
int main(int argc, char **argv) {
  ast_test_process_setup();
  if (argc != 5 && argc != 6)
    return 2;
  char *end = NULL;
  unsigned long test = strtoul(argv[1], &end, 10);
  if (!end || *end || test >= 58u)
    return 2;
  uint32_t limit = (uint32_t)strtoul(argv[2], &end, 10);
  int promote = strcmp(argv[3], "0") != 0;
  if (!end || *end || limit > 16u)
    return 2;
  Fixture f;
  AstAbiKind abi = strcmp(argv[4], "lp64") == 0    ? AST_ABI_RISCV_LP64
                   : strcmp(argv[4], "lp64f") == 0 ? AST_ABI_RISCV_LP64F
                                                   : AST_ABI_RISCV_LP64D;
  if (!open_fixture(&f, limit, promote, abi,
                    argc == 6 ? (uint32_t)strtoul(argv[5], NULL, 10) : 0u,
                    (uint32_t)test))
    return 1;
  int ok = execute_case(&f, (uint32_t)test);
  close_fixture(&f);
  return ok ? 0 : 1;
}
