#include "atomic_contract.h"
#include "physical_internal.h"
/* The concrete sequence has a complete, nonaliasing register bundle, with
 * every clobber explicit. Snapshot inputs before borrowing fixed registers;
 * restore registers before publishing the result (its home may alias one). */
int a64_move(PhysicalContext *, AstPhysicalOperand, AstPhysicalOperand);
int rv_move(PhysicalContext *, AstPhysicalOperand, AstPhysicalOperand);
static int move_for(PhysicalContext *c, AstPhysicalOperand d,
                    AstPhysicalOperand s) {
  if (c->program->target.arch == AST_TARGET_ARCH_AARCH64)
    return a64_move(c, d, s);
  if (c->program->target.arch == AST_TARGET_ARCH_RISCV64)
    return rv_move(c, d, s);
  return physical_move(c, d, s);
}
int physical_atomic_instruction(PhysicalContext *c,
                                const MachineInstruction *mi) {
  static const char *const names[] = {"load",    "store",   "rmw.xchg",
                                      "rmw.add", "rmw.sub", "rmw.and",
                                      "rmw.or",  "rmw.xor", "cas"};
  static const unsigned xr[] = {0, 10, 11, 8, 9};
  static const unsigned ar[] = {9, 10, 11, 12, 13, 14};
  static const unsigned rr[] = {5, 6, 7, 28, 29, 30, 31, 10, 11, 12};
  AstMirInstructionInfo src;
  const AstMirFunction *mir =
      ast_mir_function_at(c->program->machine->mir, c->function);
  if (!ast_mir_function_instruction(mir, mi->info.source_instruction, &src))
    return 0;
  unsigned op;
  for (op = 0; op <= AT_CAS; ++op)
    if (src.symbol.text && strlen(names[op]) == src.symbol.span.length &&
        !memcmp(src.symbol.text, names[op], src.symbol.span.length))
      break;
  if (op > AT_CAS)
    return 0;
  unsigned inputs = op == AT_LOAD ? 1u : op == AT_CAS ? 3u : 2u;
  if (mi->info.operand_count != inputs ||
      mi->info.result_count != (unsigned)(op != AT_STORE))
    return 0;
  AstPhysicalOperand values[3] = {{0}}, args[11] = {{0}};
  for (unsigned j = 0; j < inputs; ++j) {
    if (mi->info.operands[j].kind != AST_MACHINE_OPERAND_VREG)
      return 0;
    values[j] = physical_home(c, mi->info.operands[j].as.vreg);
    if (!values[j].kind)
      return 0;
  }
  unsigned bits = op == AT_LOAD
                      ? c->machine->vregs[mi->info.result_first].bit_width
                      : values[1].bit_width;
  if (bits != 1 && bits != 8 && bits != 16 && bits != 32 && bits != 64)
    return regalloc_fail(c->program, AST_REGALLOC_DIAG_UNSUPPORTED_INSTRUCTION,
                         c->function, c->instruction,
                         "Atomic storage requires bool or an 8/16/32/64-bit "
                         "scalar with natural alignment.");
  unsigned n;
  const unsigned *regs;
  AstMachineOpcode opcode;
  if (c->program->target.arch == AST_TARGET_ARCH_X86_64) {
    n = 5;
    regs = xr;
    opcode = AST_MACHINE_X86_LOCKED_ATOMIC;
  } else if (c->program->target.arch == AST_TARGET_ARCH_AARCH64) {
    n = 6;
    regs = ar;
    opcode = AST_MACHINE_A64_EXCLUSIVE_ATOMIC;
  } else {
    n = 10;
    regs = rr;
    opcode = AST_MACHINE_RISCV_LRSC_ATOMIC;
  }
  uint32_t image = physical_temp_object(c, 32, AST_FRAME_OBJECT_ABI_TEMP);
  uint32_t save = physical_temp_object(c, n * 8u, AST_FRAME_OBJECT_EMERGENCY);
  if (image == AST_MACHINE_INVALID_ID || save == AST_MACHINE_INVALID_ID)
    return 0;
  for (unsigned j = 0; j < inputs; ++j)
    if (!move_for(c, physical_frame(image, values[j].bit_width, j * 8),
                  values[j]))
      return 0;
  for (unsigned j = 0; j < n; ++j) {
    args[j] = physical_register(c, AST_MACHINE_BANK_GPR, regs[j], 64);
    if (!args[j].kind || !move_for(c, physical_frame(save, 64, j * 8), args[j]))
      return 0;
  }
  /* Last three registers: address, expected, desired. */
  for (unsigned j = 0; j < 3; ++j) {
    unsigned input = j == 0 ? 0u : j == 1 ? 1u : op == AT_CAS ? 2u : 1u;
    if (j && op == AT_LOAD) {
      if (!move_for(c, args[n - 3 + j], physical_immediate(0, 64)))
        return 0;
      continue;
    }
    AstPhysicalOperand narrow = physical_register(
        c, AST_MACHINE_BANK_GPR, regs[n - 3 + j], values[input].bit_width);
    if (!move_for(c, narrow,
                  physical_frame(image, values[input].bit_width, input * 8)))
      return 0;
  }
  args[n] = physical_immediate(op | (bits << 8u), 32);
  if (!physical_emit(c, opcode, n, args, n + 1))
    return 0;
  AstPhysicalOperand home = mi->info.result_count
                                ? physical_home(c, mi->info.result_first)
                                : (AstPhysicalOperand){0};
  if (home.kind) {
    AstPhysicalOperand result =
        physical_register(c, AST_MACHINE_BANK_GPR, regs[0], home.bit_width);
    if (!move_for(c, physical_frame(image, home.bit_width, 24), result))
      return 0;
  }
  for (unsigned j = n; j-- > 0;)
    if (!move_for(c, args[j], physical_frame(save, 64, j * 8)))
      return 0;
  return !home.kind ||
         move_for(c, home, physical_frame(image, home.bit_width, 24));
}

/* Reconstruct operation identity and ordering from immutable Machine/MIR data.
 */
int physical_atomic_source(const AstRegAllocProgram *p, uint32_t f,
                           const AstPhysicalInstructionInfo *i) {
  if (i->opcode != AST_MACHINE_X86_LOCKED_ATOMIC &&
      i->opcode != AST_MACHINE_A64_EXCLUSIVE_ATOMIC &&
      i->opcode != AST_MACHINE_RISCV_LRSC_ATOMIC)
    return 1;
  if (!atomic_physical_contract(i))
    return 0;
  const MachineFunction *mf = &p->machine->functions[f];
  if (i->source_instruction >= mf->info.instruction_count)
    return 0;
  const AstMachineInstructionInfo *mi =
      &mf->instructions[i->source_instruction].info;
  if (mi->source_op != AST_MIR_OP_ATOMIC ||
      mi->memory_order != i->memory_order ||
      mi->failure_memory_order != i->failure_memory_order)
    return 0;
  if (p->target.arch == AST_TARGET_ARCH_RISCV64 &&
      !(p->target.features & AST_TARGET_FEATURE_RV_A))
    return 0;
  AstMirInstructionInfo source;
  if (!ast_mir_function_instruction(ast_mir_function_at(p->machine->mir, f),
                                    mi->source_instruction, &source))
    return 0;
  static const char *const names[] = {"load",    "store",   "rmw.xchg",
                                      "rmw.add", "rmw.sub", "rmw.and",
                                      "rmw.or",  "rmw.xor", "cas"};
  unsigned code = (unsigned)i->operands[i->result_count].immediate_bits,
           op = code & 255u;
  if (!source.symbol.text || source.symbol.span.length != strlen(names[op]) ||
      memcmp(source.symbol.text, names[op], source.symbol.span.length))
    return 0;
  unsigned bits = op == AT_LOAD ? mf->vregs[mi->result_first].bit_width
                                : mf->vregs[mi->operands[1].as.vreg].bit_width;
  return (code >> 8u) == bits;
}
int physical_atomic_count(const AstRegAllocProgram *p, uint32_t f) {
  const MachineFunction *mf = &p->machine->functions[f];
  unsigned expected = 0;
  for (uint32_t j = 0; j < mf->info.instruction_count; ++j)
    if (mf->instructions[j].info.source_op == AST_MIR_OP_ATOMIC)
      ++expected;
  if (!expected)
    return 1; /* Per-instruction validation rejects unexpected atomics. */
  uint8_t *seen = calloc(mf->info.instruction_count, 1);
  if (!seen)
    return 0;
  unsigned actual = 0;
  int ok = 1;
  const AllocationFunction *af = &p->functions[f];
  for (uint32_t b = 0; b < af->block_count && ok; ++b)
    for (uint32_t n = 0; n < af->blocks[b].count; ++n) {
      const AstPhysicalInstructionInfo *i = &af->blocks[b].instructions[n];
      if (i->opcode != AST_MACHINE_X86_LOCKED_ATOMIC &&
          i->opcode != AST_MACHINE_A64_EXCLUSIVE_ATOMIC &&
          i->opcode != AST_MACHINE_RISCV_LRSC_ATOMIC)
        continue;
      if (!physical_atomic_source(p, f, i) || seen[i->source_instruction]++) {
        ok = 0;
        break;
      }
      ++actual;
    }
  free(seen);
  return ok && actual == expected;
}
