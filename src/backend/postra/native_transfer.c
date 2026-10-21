#include "../../isa/vector_contract.h"
#include "physical_internal.h"
#include <limits.h>

#define TRANSFER_BITS 512u

/* Bit provenance, rather than concrete test values, proves parallel input
   copies, narrowing, output caches and complete register restoration. The
   verifier owns this state; NativeLowerState is deliberately not consulted. */
typedef struct TransferByte {
  int64_t offset;
  uint32_t bits[8];
} TransferByte;
typedef struct TransferState {
  uint32_t registers[MACHINE_REGISTER_UNITS][TRANSFER_BITS];
  uint64_t numbers[MACHINE_REGISTER_UNITS];
  uint8_t number_kind[MACHINE_REGISTER_UNITS]; /* 1: integer, 2: entry SP +
                                                  offset */
  TransferByte *memory;
  uint32_t count, capacity, next;
  AstTargetArch arch;
  uint32_t sp, vector_bits;
} TransferState;

static uint32_t initial_bit(uint32_t unit, uint32_t bit) {
  return 2u + unit * TRANSFER_BITS + bit;
}
static void initialize(TransferState *s) {
  for (uint32_t u = 0; u < MACHINE_REGISTER_UNITS; ++u) {
    for (uint32_t b = 0; b < TRANSFER_BITS; ++b)
      s->registers[u][b] = ((s->arch == AST_TARGET_ARCH_RISCV64 && u == 64u) ||
                            (s->arch == AST_TARGET_ARCH_AARCH64 && u == 321u))
                               ? 0u
                               : initial_bit(u, b);
    s->number_kind[u] = 0u;
  }
  s->number_kind[s->sp] = 2u;
  s->numbers[s->sp] = 0u;
  if (s->arch == AST_TARGET_ARCH_RISCV64) {
    s->number_kind[64] = 1u;
    s->numbers[64] = 0u;
  }
  if (s->arch == AST_TARGET_ARCH_AARCH64) {
    s->number_kind[321] = 1u;
    s->numbers[321] = 0u;
  }
}
static TransferByte *memory_byte(TransferState *s, int64_t offset) {
  for (uint32_t j = 0; j < s->count; ++j)
    if (s->memory[j].offset == offset)
      return &s->memory[j];
  if (s->count == s->capacity) {
    uint32_t capacity = s->capacity ? s->capacity * 2u : 128u;
    if (capacity > 65536u)
      return NULL;
    TransferByte *memory =
        (TransferByte *)realloc(s->memory, (size_t)capacity * sizeof(*memory));
    if (!memory)
      return NULL;
    s->memory = memory;
    s->capacity = capacity;
  }
  TransferByte *byte = &s->memory[s->count++];
  byte->offset = offset;
  for (uint32_t b = 0; b < 8u; ++b)
    byte->bits[b] = s->next++;
  return byte;
}
static int address(const TransferState *s, const AstPhysicalOperand *o,
                   int64_t *out) {
  if (o->kind != AST_PHYSICAL_OPERAND_MEMORY ||
      o->register_unit >= MACHINE_REGISTER_UNITS ||
      s->number_kind[o->register_unit] != 2u || o->index_register_name[0])
    return 0;
  uint64_t raw = s->numbers[o->register_unit];
  int64_t base = raw <= INT64_MAX ? (int64_t)raw : -(int64_t)(~raw) - 1;
  if ((o->displacement > 0 && base > INT64_MAX - o->displacement) ||
      (o->displacement < 0 && base < INT64_MIN - o->displacement))
    return 0;
  *out = base + o->displacement;
  return 1;
}
static uint32_t register_bit_offset(const TransferState *s,
                                    const AstPhysicalOperand *o) {
  return s->arch == AST_TARGET_ARCH_X86_64 && o->bank == AST_MACHINE_BANK_GPR &&
                 o->bit_width == 8u && o->register_name[0] &&
                 o->register_name[1] == 'h' && !o->register_name[2]
             ? 8u
             : 0u;
}
static int read_value(TransferState *s, const AstPhysicalOperand *o,
                      uint32_t bits, uint32_t out[TRANSFER_BITS]) {
  if (!bits || bits > TRANSFER_BITS)
    return 0;
  if (o->kind == AST_PHYSICAL_OPERAND_REGISTER) {
    if (o->register_unit >= MACHINE_REGISTER_UNITS)
      return 0;
    uint32_t offset = register_bit_offset(s, o);
    if (bits > TRANSFER_BITS - offset)
      return 0;
    memcpy(out, s->registers[o->register_unit] + offset, bits * sizeof(*out));
    return 1;
  }
  if (o->kind == AST_PHYSICAL_OPERAND_IMMEDIATE && bits <= 64u) {
    for (uint32_t b = 0; b < bits; ++b)
      out[b] = (uint32_t)((o->immediate_bits >> b) & 1u);
    return 1;
  }
  int64_t offset;
  if (!address(s, o, &offset) ||
      offset > INT64_MAX - (int64_t)(TRANSFER_BITS / 8u))
    return 0;
  for (uint32_t b = 0; b < bits; ++b) {
    TransferByte *byte = memory_byte(s, offset + b / 8u);
    if (!byte)
      return 0;
    out[b] = byte->bits[b % 8u];
  }
  return 1;
}
static int write_value(TransferState *s, const AstPhysicalOperand *o,
                       uint32_t bits, const uint32_t value[TRANSFER_BITS]) {
  if (!bits || bits > TRANSFER_BITS)
    return 0;
  if (o->kind == AST_PHYSICAL_OPERAND_REGISTER) {
    uint32_t u = o->register_unit;
    if (u >= MACHINE_REGISTER_UNITS)
      return 0;
    if ((s->arch == AST_TARGET_ARCH_RISCV64 && u == 64u) ||
        (s->arch == AST_TARGET_ARCH_AARCH64 && u == 321u))
      return 1;
    uint32_t offset = register_bit_offset(s, o);
    if (bits > TRANSFER_BITS - offset)
      return 0;
    memcpy(s->registers[u] + offset, value, bits * sizeof(*value));
    /* W writes clear X[63:32]; NEON scalar writes clear the high alias. */
    uint32_t width =
        s->arch == AST_TARGET_ARCH_X86_64
            ? (u / 64u == MACHINE_PHYSREG_FILE_GPR && bits == 32u ? 64u : bits)
        : u / 64u == MACHINE_PHYSREG_FILE_GPR ? 64u
                                              : 128u;
    for (uint32_t b = bits; b < width; ++b)
      s->registers[u][b] = 0u;
    s->number_kind[u] = 0u;
    return 1;
  }
  int64_t offset;
  if (!address(s, o, &offset) ||
      offset > INT64_MAX - (int64_t)(TRANSFER_BITS / 8u))
    return 0;
  for (uint32_t b = 0; b < bits; ++b) {
    TransferByte *byte = memory_byte(s, offset + b / 8u);
    if (!byte)
      return 0;
    byte->bits[b % 8u] = value[b];
  }
  return 1;
}
static AstPhysicalOperand resolved_home(const PhysicalContext *c,
                                        uint32_t vreg) {
  AstPhysicalOperand home = physical_home((PhysicalContext *)c, vreg);
  if (home.kind == AST_PHYSICAL_OPERAND_MEMORY &&
      !strcmp(home.register_name, "@frame")) {
    if (home.register_unit >= c->fn->object_count) {
      memset(&home, 0, sizeof(home));
      return home;
    }
    home.displacement += c->fn->objects[home.register_unit].offset;
    home.register_unit =
        c->program->target.arch == AST_TARGET_ARCH_AARCH64  ? 320u
        : c->program->target.arch == AST_TARGET_ARCH_X86_64 ? 68u
                                                            : 66u;
    home.register_name[0] = 0;
  }
  return home;
}
static int step(TransferState *s, const AstPhysicalInstructionInfo *i) {
  const AstPhysicalOperand *o = i->operands;
  uint32_t value[TRANSFER_BITS] = {0}, source[TRANSFER_BITS] = {0};
  uint32_t bits = i->operand_count ? o[0].bit_width : 0u;
  if (i->opcode == AST_MACHINE_X86_STORE ||
      i->opcode == AST_MACHINE_A64_STORE || i->opcode == AST_MACHINE_RISCV_SD ||
      i->opcode == AST_MACHINE_RISCV_SW || i->opcode == AST_MACHINE_RISCV_SH ||
      i->opcode == AST_MACHINE_RISCV_SB)
    return i->operand_count == 2u &&
           read_value(s, &o[0], o[1].bit_width, value) &&
           write_value(s, &o[1], o[1].bit_width, value);
  if (i->opcode == AST_MACHINE_X86_MOV ||
      i->opcode == AST_MACHINE_X86_MOV_IMM ||
      i->opcode == AST_MACHINE_X86_LOAD || i->opcode == AST_MACHINE_A64_MOV ||
      i->opcode == AST_MACHINE_RISCV_MV || i->opcode == AST_MACHINE_RISCV_LI ||
      i->opcode == AST_MACHINE_A64_LOAD || i->opcode == AST_MACHINE_RISCV_LD ||
      i->opcode == AST_MACHINE_RISCV_LWU ||
      i->opcode == AST_MACHINE_RISCV_LHU ||
      i->opcode == AST_MACHINE_RISCV_LBU) {
    if (i->operand_count != 2u || o[0].kind != AST_PHYSICAL_OPERAND_REGISTER)
      return 0;
    uint32_t read_bits =
        o[1].kind == AST_PHYSICAL_OPERAND_MEMORY ? o[1].bit_width : bits;
    if (!read_value(s, &o[1], read_bits, value))
      return 0;
    uint8_t kind = o[1].kind == AST_PHYSICAL_OPERAND_REGISTER
                       ? s->number_kind[o[1].register_unit]
                   : o[1].kind == AST_PHYSICAL_OPERAND_IMMEDIATE ? 1u
                                                                 : 0u;
    uint64_t number = o[1].kind == AST_PHYSICAL_OPERAND_REGISTER
                          ? s->numbers[o[1].register_unit]
                          : o[1].immediate_bits;
    uint32_t write_bits =
        s->arch == AST_TARGET_ARCH_X86_64 ? bits
        : o[0].bank == AST_MACHINE_BANK_GPR
            ? (s->arch == AST_TARGET_ARCH_AARCH64 && bits <= 32u ? 32u : 64u)
            : bits;
    if (!write_value(s, &o[0], write_bits, value))
      return 0;
    if (bits == 64u) {
      s->number_kind[o[0].register_unit] = kind;
      s->numbers[o[0].register_unit] = number;
    }
    return 1;
  }
  if (i->opcode == AST_MACHINE_A64_MOVZ || i->opcode == AST_MACHINE_A64_MOVK) {
    if (i->operand_count != 3u || bits > 64u || o[2].immediate_bits >= bits ||
        o[2].immediate_bits % 16u)
      return 0;
    if (i->opcode == AST_MACHINE_A64_MOVK && !read_value(s, &o[0], bits, value))
      return 0;
    uint32_t shift = (uint32_t)o[2].immediate_bits;
    for (uint32_t b = 0; b < 16u; ++b)
      value[shift + b] = (uint32_t)((o[1].immediate_bits >> b) & 1u);
    return write_value(s, &o[0], bits, value);
  }
  if (i->opcode == AST_MACHINE_A64_UBFM) {
    if (i->operand_count != 4u || o[2].immediate_bits ||
        o[3].immediate_bits >= bits || !read_value(s, &o[1], bits, source))
      return 0;
    for (uint32_t b = 0; b <= o[3].immediate_bits; ++b)
      value[b] = source[b];
    return write_value(s, &o[0], bits, value);
  }
  if (i->opcode == AST_MACHINE_RISCV_SLLI ||
      i->opcode == AST_MACHINE_RISCV_SRLI) {
    if (i->operand_count != 3u || o[2].immediate_bits >= 64u ||
        !read_value(s, &o[1], 64u, source))
      return 0;
    uint32_t shift = (uint32_t)o[2].immediate_bits;
    for (uint32_t b = 0; b < 64u; ++b)
      if (i->opcode == AST_MACHINE_RISCV_SLLI ? b >= shift : b + shift < 64u)
        value[b] =
            source[i->opcode == AST_MACHINE_RISCV_SLLI ? b - shift : b + shift];
    return write_value(s, &o[0], 64u, value);
  }
  if (i->opcode == AST_MACHINE_A64_LEA || i->opcode == AST_MACHINE_RISCV_LA) {
    int64_t offset;
    if (i->operand_count != 2u || !address(s, &o[1], &offset))
      return 0;
    for (uint32_t b = 0; b < 64u; ++b)
      value[b] = s->next++;
    if (!write_value(s, &o[0], 64u, value))
      return 0;
    s->number_kind[o[0].register_unit] = 2u;
    s->numbers[o[0].register_unit] = (uint64_t)offset;
    return 1;
  }
  if (i->opcode == AST_MACHINE_A64_ADD || i->opcode == AST_MACHINE_A64_SUB ||
      i->opcode == AST_MACHINE_RISCV_ADD ||
      i->opcode == AST_MACHINE_RISCV_ADDI) {
    if (i->operand_count != 3u || o[1].kind != AST_PHYSICAL_OPERAND_REGISTER)
      return 0;
    uint8_t left = s->number_kind[o[1].register_unit];
    uint8_t right = o[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE
                        ? 1u
                        : s->number_kind[o[2].register_unit];
    uint64_t number = o[2].kind == AST_PHYSICAL_OPERAND_IMMEDIATE
                          ? o[2].immediate_bits
                          : s->numbers[o[2].register_unit];
    if (!left || right != 1u)
      return 0;
    number = i->opcode == AST_MACHINE_A64_SUB
                 ? s->numbers[o[1].register_unit] - number
                 : s->numbers[o[1].register_unit] + number;
    for (uint32_t b = 0; b < 64u; ++b)
      value[b] = left == 1u ? (uint32_t)((number >> b) & 1u) : s->next++;
    if (!write_value(s, &o[0], 64u, value))
      return 0;
    s->number_kind[o[0].register_unit] = left;
    s->numbers[o[0].register_unit] = number;
    return 1;
  }
  return 0;
}
static int restored(const TransferState *s) {
  if (s->number_kind[s->sp] != 2u || s->numbers[s->sp])
    return 0;
  for (uint32_t u = 0; u < MACHINE_REGISTER_UNITS; ++u) {
    if (u == s->sp || (s->arch == AST_TARGET_ARCH_RISCV64 && u == 64u))
      continue;
    uint32_t bits = u / 64u == MACHINE_PHYSREG_FILE_GPR         ? 64u
                    : u / 64u == MACHINE_PHYSREG_FILE_FP_VECTOR ? s->vector_bits
                                                                : 0u;
    for (uint32_t b = 0; b < bits; ++b)
      if (s->registers[u][b] != initial_bit(u, b))
        return 0;
  }
  return 1;
}
int physical_verify_native_transfers(PhysicalContext *c,
                                     const AstMirInstructionInfo *source,
                                     const NativeBindings *bindings,
                                     const PhysicalBlock *block, uint32_t begin,
                                     uint32_t start, uint32_t end,
                                     uint32_t tail) {
  TransferState *s = (TransferState *)calloc(1u, sizeof(*s));
  if (!s)
    return 0;
  s->arch = c->program->target.arch;
  s->sp = s->arch == AST_TARGET_ARCH_AARCH64  ? 320u
          : s->arch == AST_TARGET_ARCH_X86_64 ? 68u
                                              : 66u;
  s->vector_bits =
      s->arch == AST_TARGET_ARCH_AARCH64                             ? 128u
      : s->arch != AST_TARGET_ARCH_X86_64                            ? 0u
      : c->program->target.features & AST_TARGET_FEATURE_X86_AVX512F ? 512u
      : c->program->target.features & AST_TARGET_FEATURE_X86_AVX     ? 256u
                                                                     : 128u;
  s->next = 2u + MACHINE_REGISTER_UNITS * TRANSFER_BITS;
  initialize(s);
  uint32_t inputs[14][TRANSFER_BITS] = {{0}},
           outputs[14][TRANSFER_BITS] = {{0}};
  int ok = bindings->input_count <= 14u && bindings->output_count <= 14u;
  const MachineInstruction *mi = &c->machine->instructions[c->instruction];
  for (uint32_t j = 0; ok && j < bindings->input_count; ++j) {
    AstPhysicalOperand home = resolved_home(c, mi->info.operands[j].as.vreg);
    ok = read_value(s, &home, bindings->inputs[j].bit_width, inputs[j]);
  }
  for (uint32_t k = begin; ok && k < start; ++k)
    ok = step(s, &block->instructions[k]);
  for (uint32_t j = 0; ok && j < bindings->input_count; ++j) {
    uint32_t actual[TRANSFER_BITS];
    const AstPhysicalOperand *input = &bindings->inputs[j];
    uint32_t width =
        input->bank == AST_MACHINE_BANK_GPR && s->arch != AST_TARGET_ARCH_X86_64
            ? 64u
            : input->bit_width;
    ok = read_value(s, input, width, actual);
    for (uint32_t b = 0; ok && b < width; ++b)
      if (actual[b] != inputs[j][b])
        ok = 0;
  }
  /* The body has already been reconstructed from HIR. Distinct symbols for
     its definitions make routing/preservation proofs value-independent. */
  for (uint32_t k = start; ok && k < end; ++k) {
    const AstPhysicalInstructionInfo *i = &block->instructions[k];
    for (uint32_t d = 0; ok && d < i->result_count; ++d) {
      uint32_t value[TRANSFER_BITS];
      const AstPhysicalOperand *dst = &i->operands[d];
      const VectorForm *vector = vector_physical_form(i);
      uint32_t bits =
          dst->bank == AST_MACHINE_BANK_GPR ? dst->bit_width
          : s->arch == AST_TARGET_ARCH_X86_64
              ? (vector && (vector_vex(vector) || vector_evex(vector))
                     ? s->vector_bits
                     : dst->bit_width)
              : 128u;
      for (uint32_t b = 0; b < bits; ++b)
        value[b] = s->next++;
      ok = write_value(s, dst, bits, value);
    }
  }
  for (uint32_t j = 0; ok && j < bindings->output_count; ++j)
    ok = read_value(s, &bindings->outputs[j], bindings->outputs[j].bit_width,
                    outputs[j]);
  for (uint32_t k = end; ok && k < tail; ++k)
    ok = step(s, &block->instructions[k]);
  ok = ok && restored(s);
  /* Reconstruct each result's final destination from allocation/liveness;
     read its cached value without consulting the lowering cache map. */
  for (uint32_t id = 0; ok && id < c->machine->info.instruction_count; ++id) {
    const MachineInstruction *result = &c->machine->instructions[id];
    AstMirInstructionInfo rs;
    if (result->info.source_op != AST_MIR_OP_NATIVE_RESULT)
      continue;
    if (!ast_mir_function_instruction(
            ast_mir_function_at(c->program->machine->mir, c->function),
            result->info.source_instruction, &rs)) {
      ok = 0;
      break;
    }
    if (rs.native_region != source->native_region)
      continue;
    if (rs.aux_index >= bindings->output_count ||
        result->info.result_count != 1u) {
      ok = 0;
      break;
    }
    initialize(s);
    PhysicalContext rc = *c;
    rc.instruction = id;
    AstPhysicalOperand home = resolved_home(&rc, result->info.result_first);
    if (!home.kind)
      continue; /* A dead result needs no transfer. */
    uint32_t matches = 0u;
    for (uint32_t b = 0; ok && b < c->fn->block_count; ++b)
      for (uint32_t k = 0; ok && k < c->fn->blocks[b].count; ++k)
        if (c->fn->blocks[b].instructions[k].source_instruction == id) {
          ++matches;
          ok = step(s, &c->fn->blocks[b].instructions[k]);
        }
    uint32_t actual[TRANSFER_BITS],
        bits = bindings->outputs[rs.aux_index].bit_width;
    ok = ok && matches && read_value(s, &home, bits, actual) &&
         !memcmp(actual, outputs[rs.aux_index], bits * sizeof(*actual));
  }
  free(s->memory);
  free(s);
  return ok;
}
