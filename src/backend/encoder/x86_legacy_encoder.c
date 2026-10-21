#include "../../core/parallel.h"
#include "../postra/atomic_contract.h"
#include "encoder_internal.h"

/* The legacy backend uses a stable stack home for every MIR value. This keeps
 * address-taken objects and parallel CFG copies independent of register
 * pressure. EAX/ECX/EDX are scratch and the encoder deliberately avoids
 * touching EBX/ESI/EDI, so only EBP needs the cdecl callee-save prologue. In
 * code16 both pointers and the calling convention remain 32-bit (386+). */
typedef struct Legacy {
  EncoderBuilder b;
  const AstMirFunction *fn;
  AstMirFunctionInfo info;
  AstMirVRegInfo *values;
  int32_t *homes, *slots;
  size_t *blocks;
  uint8_t *dead_insts, *dead_values;
  int32_t edge_home;
  uint32_t frame_size;
  uint32_t current_block;
  uint32_t *layout, layout_count, next_block;
  size_t last_store_end;
  int32_t last_store_offset;
  unsigned last_store_reg, last_store_bits;
  uint8_t last_store_valid, allow_home_forward;
  int mode16;
  int has_sret;
  uint32_t sret_size;
} Legacy;

enum { AX, CX, DX, BX, SP, BP, SI, DI };
static int byte(Legacy *c, unsigned v) { return encoder_le(&c->b, v, 1); }
static int word32(Legacy *c, uint32_t v) { return encoder_le(&c->b, v, 4); }
static int fail(Legacy *c, const char *s) {
  return encoder_fail(&c->b, AST_ENCODER_DIAG_UNSUPPORTED_INSTRUCTION, s);
}
static int prefix(Legacy *c, unsigned bits, int memory) {
  return (!(bits != 8 && ((bits == 16) != c->mode16)) || byte(c, 0x66)) &&
         (!(memory && c->mode16) || byte(c, 0x67));
}
static int rr(Legacy *c, unsigned opcode, unsigned dst, unsigned src) {
  if (opcode == 0x89u && dst == src)
    return 1; /* Legacy mode: no upper-64 clearing. */
  return prefix(c, 32, 0) && byte(c, opcode) &&
         byte(c, 0xc0u | (src << 3) | dst);
}
static int imm(Legacy *c, unsigned reg, uint32_t value) {
  return prefix(c, 32, 0) && byte(c, 0xb8u + reg) && word32(c, value);
}
static int mem_tail(Legacy *c, unsigned field, unsigned base,
                    int32_t displacement) {
  unsigned mod = !displacement && base != BP                            ? 0u
                 : displacement >= INT8_MIN && displacement <= INT8_MAX ? 1u
                                                                        : 2u;
  if (!byte(c, (mod << 6u) | (field << 3u) | base) ||
      (base == SP && !byte(c, 0x24u)))
    return 0;
  return !mod || encoder_le(&c->b, (uint32_t)displacement, mod == 1u ? 1u : 4u);
}
static int load(Legacy *c, unsigned reg, unsigned base, int32_t off,
                unsigned bits, int sign) {
  if (c->allow_home_forward && base == BP && c->last_store_valid &&
      c->last_store_end == c->b.out->size && c->last_store_offset == off &&
      c->last_store_bits == bits) {
    if (bits == 32u)
      return rr(c, 0x89u, reg, c->last_store_reg);
    if (bits == 16u || (bits == 8u && c->last_store_reg < 4u))
      return prefix(c, 32u, 0) && byte(c, 0x0fu) &&
             byte(c, (sign ? 0xbeu : 0xb6u) + (bits == 16u)) &&
             byte(c, 0xc0u | (reg << 3u) | c->last_store_reg);
  }
  if (bits <= 16)
    return prefix(c, 32, 1) && byte(c, 0x0f) &&
           byte(c, (sign ? 0xbeu : 0xb6u) + (bits > 8)) &&
           mem_tail(c, reg, base, off);
  return prefix(c, 32, 1) && byte(c, 0x8b) && mem_tail(c, reg, base, off);
}
static int store(Legacy *c, unsigned reg, unsigned base, int32_t off,
                 unsigned bits) {
  if (c->allow_home_forward && base == BP && c->last_store_valid &&
      c->last_store_end == c->b.out->size && c->last_store_offset == off &&
      c->last_store_bits == bits && c->last_store_reg == reg)
    return 1;
  int ok = prefix(c, bits, 1) && byte(c, bits <= 8 ? 0x88 : 0x89) &&
           mem_tail(c, reg, base, off);
  c->last_store_valid = (uint8_t)(ok && c->allow_home_forward && base == BP);
  c->last_store_end = c->b.out->size;
  c->last_store_offset = off;
  c->last_store_bits = bits;
  c->last_store_reg = reg;
  return ok;
}
static int lea(Legacy *c, unsigned reg, unsigned base, int32_t off) {
  return prefix(c, 32, 1) && byte(c, 0x8d) && mem_tail(c, reg, base, off);
}
static int group_imm(Legacy *c, unsigned group, unsigned reg, uint32_t value) {
  int compact = value == (uint32_t)(int32_t)(int8_t)(uint8_t)value;
  return prefix(c, 32, 0) && byte(c, compact ? 0x83u : 0x81u) &&
         byte(c, 0xc0u | (group << 3u) | reg) &&
         encoder_le(&c->b, value, compact ? 1u : 4u);
}
static int unary_reg(Legacy *c, unsigned group, unsigned reg) {
  return prefix(c, 32, 0) && byte(c, 0xf7) &&
         byte(c, 0xc0u | (group << 3) | reg);
}
static int shift(Legacy *c, unsigned group, unsigned reg, unsigned amount) {
  return prefix(c, 32, 0) && byte(c, 0xc1) &&
         byte(c, 0xc0u | (group << 3) | reg) && byte(c, amount);
}
static int shift_cl(Legacy *c, unsigned group, unsigned reg) {
  return prefix(c, 32, 0) && byte(c, 0xd3) &&
         byte(c, 0xc0u | (group << 3) | reg);
}
static int double_shift_cl(Legacy *c, int right, unsigned dst, unsigned src) {
  /* SHLD/SHRD r/m32,r32,CL.  They are available on every 386+ CPU, which is
   * the minimum CPU for both legacy modes. */
  return prefix(c, 32, 0) && byte(c, 0x0f) && byte(c, right ? 0xadu : 0xa5u) &&
         byte(c, 0xc0u | (src << 3) | dst);
}
static int double_shift_imm(Legacy *c, int right, unsigned dst, unsigned src,
                            unsigned amount) {
  return prefix(c, 32, 0) && byte(c, 0x0f) && byte(c, right ? 0xacu : 0xa4u) &&
         byte(c, 0xc0u | (src << 3) | dst) && byte(c, amount);
}
static int test_reg(Legacy *c, unsigned reg) { return rr(c, 0x85u, reg, reg); }
static int push(Legacy *c, unsigned reg) {
  return prefix(c, 32, 0) && byte(c, 0x50u + reg);
}
static int pop(Legacy *c, unsigned reg) {
  return prefix(c, 32, 0) && byte(c, 0x58u + reg);
}
static unsigned size_bits(uint64_t size) {
  return size >= 4 ? 32 : size >= 2 ? 16 : 8;
}
static uint32_t value_size(Legacy *c, AstMirVReg v) {
  return (uint32_t)ast_sema_type_size(c->values[v].type);
}
static int get(Legacy *c, AstMirVReg v, unsigned reg, uint32_t part, int sign) {
  uint32_t size = value_size(c, v);
  if (part >= size)
    return imm(c, reg, 0);
  return load(c, reg, BP, c->homes[v] + (int32_t)part, size_bits(size - part),
              sign);
}
static int put(Legacy *c, AstMirVReg v, unsigned reg, uint32_t part) {
  uint32_t size = value_size(c, v);
  if (part >= size)
    return 1;
  return store(c, reg, BP, c->homes[v] + (int32_t)part, size_bits(size - part));
}
static int copy(Legacy *c, unsigned dstbase, int32_t dst, unsigned srcbase,
                int32_t src, uint32_t size) {
  if (c->allow_home_forward && dstbase == BP && srcbase == BP && dst == src)
    return 1;
  for (uint32_t off = 0; off < size;) {
    unsigned bits = size_bits(size - off);
    if (!load(c, AX, srcbase, src + (int32_t)off, bits, 0) ||
        !store(c, AX, dstbase, dst + (int32_t)off, bits))
      return 0;
    off += bits / 8;
  }
  return 1;
}
/* NEG/SBB creates a 0/-1 mask on the baseline 386 ISA; no CMOV feature is
   assumed in code16/i386. Values are already evaluated private stack images,
   so selecting their scalar bits does not speculate source memory effects. */
static int scalar_select(Legacy *c, AstMirVReg result, AstMirVReg condition,
                         AstMirVReg yes, AstMirVReg no) {
  uint32_t size = value_size(c, result);
  if (!get(c, condition, CX, 0u, 0) || !unary_reg(c, 3u, CX) ||
      !rr(c, 0x19u, CX, CX))
    return 0;
  for (uint32_t off = 0u; off < size; off += 4u)
    if (!get(c, no, AX, off, 0) || !get(c, yes, DX, off, 0) ||
        !rr(c, 0x31u, DX, AX) || !rr(c, 0x21u, DX, CX) ||
        !rr(c, 0x31u, AX, DX) || !put(c, result, AX, off))
      return 0;
  return 1;
}
static int symbol(Legacy *c, unsigned reg, AstName module, AstName name) {
  return prefix(c, 32, 0) && byte(c, 0xb8u + reg) && word32(c, 0) &&
         encoder_fixup(&c->b, c->b.out->size - 4, 4, AST_ENCODER_FIXUP_ABS32,
                       module, name);
}
static size_t jump(Legacy *c, int condition) {
  if (!prefix(c, 32, 0) || (condition >= 0 && !byte(c, 0x0f)) ||
      !byte(c, condition < 0 ? 0xe9u : 0x80u + (unsigned)condition))
    return SIZE_MAX;
  size_t at = c->b.out->size;
  return word32(c, 0) ? at : SIZE_MAX;
}
static int patch(Legacy *c, size_t at, size_t to) {
  c->last_store_valid =
      0u; /* A patch point can be reached from another path. */
  if (at == SIZE_MAX || to > INT32_MAX || at > INT32_MAX)
    return fail(c, "Legacy branch exceeds rel32 range.");
  uint32_t delta = (uint32_t)(int32_t)((int64_t)to - (int64_t)(at + 4));
  for (unsigned n = 0; n < 4; ++n)
    c->b.out->bytes[at + n] = (uint8_t)(delta >> (8 * n));
  return 1;
}
static int setcc(Legacy *c, unsigned condition) {
  return byte(c, 0x0f) && byte(c, 0x90u + condition) && byte(c, 0xc0) &&
         prefix(c, 32, 0) && byte(c, 0x0f) && byte(c, 0xb6) && byte(c, 0xc0);
}
static int bit_scan(Legacy *c, int reverse, unsigned dst, unsigned src) {
  return prefix(c, 32, 0) && byte(c, 0x0f) &&
         byte(c, reverse ? 0xbdu : 0xbcu) && byte(c, 0xc0u | (dst << 3) | src);
}
static int rotate_cl_width(Legacy *c, int right, unsigned reg, unsigned bits) {
  if (bits != 8u && bits != 16u && bits != 32u)
    return fail(c, "Invalid legacy rotate width.");
  return prefix(c, bits, 0) && byte(c, bits == 8u ? 0xd2u : 0xd3u) &&
         byte(c, 0xc0u | ((right ? 1u : 0u) << 3) | reg);
}
static int rotate_imm_width(Legacy *c, int right, unsigned reg, unsigned bits,
                            unsigned amount) {
  if (bits != 8u && bits != 16u && bits != 32u)
    return fail(c, "Invalid legacy rotate width.");
  return prefix(c, bits, 0) && byte(c, 0xc1u) &&
         byte(c, 0xc0u | ((right ? 1u : 0u) << 3) | reg) && byte(c, amount);
}
static int store_bool_home(Legacy *c, int32_t home, uint32_t offset,
                           unsigned reg) {
  return store(c, reg, BP, home + (int32_t)offset, 8u);
}

static int constant_vreg_u64(Legacy *c, AstMirVReg v, uint64_t *out) {
  if (!c || !out)
    return 0;
  for (uint32_t id = 0; id < c->info.instruction_count; ++id) {
    AstMirInstructionInfo i;
    if (!ast_mir_function_instruction(c->fn, id, &i))
      return 0;
    if (i.result != v || i.op != AST_MIR_OP_CONSTANT ||
        i.constant.kind == AST_SEMA_CONST_NONE || i.constant.is_negative ||
        i.constant.hi)
      continue;
    *out = i.constant.lo;
    return 1;
  }
  return 0;
}

static int aggregate_element_offset(const AstSemaType *base, uint64_t index,
                                    uint64_t *offset,
                                    const AstSemaType **element) {
  if (!base || !offset || !element)
    return 0;
  AstSemaTypeKind k = ast_sema_type_kind(base);
  if (k == AST_SEMA_TYPE_ARRAY) {
    uint64_t count = 0;
    const AstSemaType *e = ast_sema_type_element(base);
    uint64_t es = e ? ast_sema_type_size(e) : UINT64_MAX;
    if (!e || !ast_sema_type_array_count(base, &count) || index >= count ||
        es == UINT64_MAX || (es && index > UINT64_MAX / es))
      return 0;
    *offset = index * es;
    *element = e;
    return 1;
  }
  if (k == AST_SEMA_TYPE_TUPLE) {
    uint32_t count = ast_sema_type_tuple_count(base);
    if (index >= count)
      return 0;
    uint64_t at = 0;
    for (uint32_t n = 0; n < count; ++n) {
      const AstSemaType *e = ast_sema_type_tuple_item(base, n);
      uint64_t es = e ? ast_sema_type_size(e) : UINT64_MAX;
      uint32_t a = e ? ast_sema_type_alignment(e) : 0u;
      if (!e || es == UINT64_MAX || !a)
        return 0;
      uint64_t pad = (a - (at % a)) % a;
      if (at > UINT64_MAX - pad)
        return 0;
      at += pad;
      if (n == index) {
        *offset = at;
        *element = e;
        return 1;
      }
      if (at > UINT64_MAX - es)
        return 0;
      at += es;
    }
  }
  return 0;
}

static int struct_member_offset(const AstSemaType *base, uint32_t index,
                                uint64_t *offset, const AstSemaType **element) {
  if (!base || ast_sema_type_kind(base) != AST_SEMA_TYPE_STRUCT || !offset ||
      !element || index >= ast_sema_type_struct_field_count(base))
    return 0;
  AstSemaFieldInfo field;
  if (!ast_sema_type_struct_field(base, index, &field) || !field.type)
    return 0;
  *offset = field.offset;
  *element = field.type;
  return 1;
}

static int aggregate_extract(Legacy *c, const AstMirInstructionInfo *i,
                             int member) {
  if (!i || i->result == AST_MIR_INVALID_ID || i->operand_count < 1u)
    return 0;
  AstMirVReg basev = i->operands[0];
  const AstSemaType *base = c->values[basev].type, *element = NULL;
  uint64_t off = 0;
  if (member) {
    if (!struct_member_offset(base, i->aux_index, &off, &element))
      return fail(c, "Invalid legacy aggregate member access.");
  } else {
    if (i->operand_count != 2u)
      return 0;
    uint64_t index = 0;
    if (!constant_vreg_u64(c, i->operands[1], &index))
      return fail(c, "Legacy aggregate index requires a compile-time index "
                     "after semantic validation.");
    if (!aggregate_element_offset(base, index, &off, &element))
      return fail(c, "Invalid legacy aggregate index access.");
  }
  uint64_t es = ast_sema_type_size(element);
  uint64_t bs = ast_sema_type_size(base);
  if (es == UINT64_MAX || bs == UINT64_MAX || off > bs || es > bs - off ||
      es > UINT32_MAX)
    return fail(
        c, "Legacy aggregate element has no fixed storage representation.");
  return copy(c, BP, c->homes[i->result], BP, c->homes[basev] + (int32_t)off,
              (uint32_t)es);
}

static int aggregate_pack(Legacy *c, const AstMirInstructionInfo *i) {
  if (!c || !i || i->result == AST_MIR_INVALID_ID || !i->type ||
      ast_sema_type_kind(i->type) != AST_SEMA_TYPE_TUPLE ||
      ast_sema_type_tuple_count(i->type) != i->operand_count) {
    return fail(c, "Invalid legacy aggregate pack.");
  }
  uint64_t total = ast_sema_type_size(i->type);
  if (total == UINT64_MAX || total > UINT32_MAX)
    return fail(c, "Legacy packed aggregate has no fixed representation.");
  /* Homes are not guaranteed to start zeroed.  Clear the complete tuple so
   * ABI-visible padding is deterministic before copying each element at the
   * semantic layout offset. */
  if (!imm(c, AX, 0u))
    return 0;
  for (uint32_t off = 0u; off < (uint32_t)total;) {
    unsigned bits = size_bits((uint32_t)total - off);
    if (!store(c, AX, BP, c->homes[i->result] + (int32_t)off, bits))
      return 0;
    off += bits / 8u;
  }
  for (uint32_t n = 0u; n < i->operand_count; ++n) {
    uint64_t off = 0u;
    const AstSemaType *element = NULL;
    if (!aggregate_element_offset(i->type, n, &off, &element) || !element)
      return fail(c, "Invalid legacy tuple layout.");
    uint64_t bytes = ast_sema_type_size(element);
    if (bytes == UINT64_MAX || off > total || bytes > total - off ||
        bytes > UINT32_MAX)
      return fail(c, "Legacy tuple element has no fixed representation.");
    if (!copy(c, BP, c->homes[i->result] + (int32_t)off, BP,
              c->homes[i->operands[n]], (uint32_t)bytes))
      return 0;
  }
  return 1;
}

static int legacy_fence(Legacy *c, AstMemoryOrder order) {
  /* IA-32 TSO already provides acquire/release ordering for ordinary loads
   * and stores.  A seq_cst/acq_rel standalone fence uses a locked no-op on
   * the current stack word, available on the 80386 baseline. */
  if (order == AST_MEMORY_ORDER_NONE || order == AST_MEMORY_ORDER_RELAXED ||
      order == AST_MEMORY_ORDER_ACQUIRE || order == AST_MEMORY_ORDER_RELEASE)
    return 1;
  return byte(c, 0xf0u) && prefix(c, 32, 1) && byte(c, 0x83u) &&
         mem_tail(c, 0u, SP, 0) && byte(c, 0u); /* lock add dword [esp],0 */
}

static int fp_mem_base(Legacy *c, unsigned opcode, unsigned group,
                       unsigned base, int32_t off) {
  return (!c->mode16 || byte(c, 0x67)) && byte(c, opcode) &&
         mem_tail(c, group, base, off);
}
static int fp_mem(Legacy *c, unsigned opcode, unsigned group, int32_t home) {
  return fp_mem_base(c, opcode, group, BP, home);
}
static int fp_load(Legacy *c, AstMirVReg value) {
  return fp_mem(c, value_size(c, value) == 4 ? 0xd9u : 0xddu, 0,
                c->homes[value]);
}
static int fp_store(Legacy *c, AstMirVReg value) {
  return fp_mem(c, value_size(c, value) == 4 ? 0xd9u : 0xddu, 3,
                c->homes[value]);
}
static int fp_fild64(Legacy *c, unsigned base, int32_t off) {
  return fp_mem_base(c, 0xdfu, 5u, base, off);
}
static int fp_fistp64(Legacy *c, unsigned base, int32_t off) {
  return fp_mem_base(c, 0xdfu, 7u, base, off);
}
static int fp_fnstcw(Legacy *c, unsigned base, int32_t off) {
  return fp_mem_base(c, 0xd9u, 7u, base, off);
}
static int fp_fldcw(Legacy *c, unsigned base, int32_t off) {
  return fp_mem_base(c, 0xd9u, 5u, base, off);
}
static int condition(AstTokenKind op, int sign) {
  switch (op) {
  case AST_TOK_EQ_EQ:
    return 4;
  case AST_TOK_BANG_EQ:
    return 5;
  case AST_TOK_LT:
    return sign ? 12 : 2;
  case AST_TOK_LT_EQ:
    return sign ? 14 : 6;
  case AST_TOK_GT:
    return sign ? 15 : 7;
  case AST_TOK_GT_EQ:
    return sign ? 13 : 3;
  default:
    return -1;
  }
}
static int compare(Legacy *c, const AstMirInstructionInfo *i) {
  AstMirVReg a = i->operands[0], b = i->operands[1];
  int sign = ast_sema_type_is_signed(c->values[a].type);
  int cc = condition(i->token_op, sign);
  if (value_size(c, a) > 4) {
    if (!get(c, a, AX, 4, 0) || !get(c, b, CX, 4, 0) || !rr(c, 0x39, AX, CX))
      return 0;
    size_t equal = jump(c, 4);
    if (!setcc(c, (unsigned)cc))
      return 0;
    size_t done = jump(c, -1);
    if (!patch(c, equal, c->b.out->size) || !get(c, a, AX, 0, 0) ||
        !get(c, b, CX, 0, 0) || !rr(c, 0x39, AX, CX) ||
        !setcc(c, (unsigned)condition(i->token_op, 0)) ||
        !patch(c, done, c->b.out->size))
      return 0;
  } else if (!get(c, a, AX, 0, sign) || !get(c, b, CX, 0, sign) ||
             !rr(c, 0x39, AX, CX) || !setcc(c, (unsigned)cc))
    return 0;
  return put(c, i->result, AX, 0);
}
static int float_binary(Legacy *c, const AstMirInstructionInfo *i) {
  AstMirVReg a = i->operands[0], b = i->operands[1];
  int cc = condition(i->token_op, 0);
  unsigned group = i->token_op == AST_TOK_PLUS    ? 0u
                   : i->token_op == AST_TOK_STAR  ? 1u
                   : i->token_op == AST_TOK_MINUS ? 4u
                   : i->token_op == AST_TOK_SLASH ? 6u
                                                  : 8u;
  if (cc >= 0)
    group = 3; /* FCOMP pops ST(0). */
  if (group == 8)
    return fail(c, "Unsupported legacy floating operation.");
  if (!fp_load(c, a) ||
      !fp_mem(c, value_size(c, b) == 4 ? 0xd8u : 0xdcu, group, c->homes[b]))
    return 0;
  if (cc < 0)
    return fp_store(c, i->result);
  /* The x87 unordered result sets ZF/PF/CF. Only != accepts unordered. */
  if (!byte(c, 0xdf) || !byte(c, 0xe0) || !byte(c, 0x9e) ||
      !setcc(c, (unsigned)cc) || !byte(c, 0x0f) ||
      !byte(c, i->token_op == AST_TOK_BANG_EQ ? 0x9a : 0x9b) ||
      !byte(c, 0xc2) ||
      !byte(c, i->token_op == AST_TOK_BANG_EQ ? 0x08 : 0x20) || !byte(c, 0xd0))
    return 0;
  return put(c, i->result, AX, 0);
}
/* Negate one little-endian 64-bit pair in memory using EDX:EAX. */
static int negate64_mem(Legacy *c, unsigned base, int32_t off) {
  return load(c, AX, base, off, 32u, 0) && load(c, DX, base, off + 4, 32u, 0) &&
         unary_reg(c, 3u, AX) && group_imm(c, 2u, DX, 0u) &&
         unary_reg(c, 3u, DX) && store(c, AX, base, off, 32u) &&
         store(c, DX, base, off + 4, 32u);
}

/* Force the architectural integer-divide exception without requiring an OS or
 * runtime helper.  Both DIV and IDIV report divide-by-zero as #DE. */
static int division_trap(Legacy *c) {
  return imm(c, AX, 0u) && imm(c, DX, 0u) && imm(c, CX, 0u) &&
         unary_reg(c, 6u, CX);
}

/* Self-contained 64/64 -> 64 long division for the 386+ legacy targets.
 * Scratch memory is temporary ESP-relative storage, so EBP-based MIR homes are
 * stable for the whole operation.  No libgcc/compiler-rt symbol is introduced,
 * which is required for freestanding/bare-metal output. */
static int divide64_mem(Legacy *c, unsigned abase, int32_t ahome,
                        unsigned bbase, int32_t bhome, int sign, int remainder,
                        int trap_signed_overflow, unsigned dstbase,
                        int32_t dsthome) {
  enum { D = 0, V = 8, R = 16, Q = 24, SCRATCH = 32 };

  if (!group_imm(c, 5u, SP, SCRATCH) || !copy(c, SP, D, abase, ahome, 8u) ||
      !copy(c, SP, V, bbase, bhome, 8u))
    return 0;

  /* Match IA-32/x86-64 integer divide failure for a dynamic signed
   * INT64_MIN / -1.  (The compile-time form is rejected by Sema.) */
  if (sign && !remainder && trap_signed_overflow) {
    size_t no_overflow[4];
    if (!load(c, AX, abase, ahome + 4, 32u, 0) ||
        !group_imm(c, 7u, AX, 0x80000000u))
      return 0;
    no_overflow[0] = jump(c, 5u);
    if (!load(c, AX, abase, ahome, 32u, 0) || !group_imm(c, 7u, AX, 0u))
      return 0;
    no_overflow[1] = jump(c, 5u);
    if (!load(c, AX, bbase, bhome + 4, 32u, 0) ||
        !group_imm(c, 7u, AX, UINT32_MAX))
      return 0;
    no_overflow[2] = jump(c, 5u);
    if (!load(c, AX, bbase, bhome, 32u, 0) || !group_imm(c, 7u, AX, UINT32_MAX))
      return 0;
    no_overflow[3] = jump(c, 5u);
    if (!division_trap(c))
      return 0;
    for (unsigned n = 0; n < 4; ++n)
      if (!patch(c, no_overflow[n], c->b.out->size))
        return 0;
  }

  if (sign) {
    if (!load(c, AX, SP, D + 4, 32u, 0) || !test_reg(c, AX))
      return 0;
    size_t d_positive = jump(c, 9u); /* JNS */
    if (!negate64_mem(c, SP, D) || !patch(c, d_positive, c->b.out->size))
      return 0;
    if (!load(c, AX, SP, V + 4, 32u, 0) || !test_reg(c, AX))
      return 0;
    size_t v_positive = jump(c, 9u);
    if (!negate64_mem(c, SP, V) || !patch(c, v_positive, c->b.out->size))
      return 0;
  }

  /* Explicit zero check: the restoring algorithm itself would otherwise
   * yield all-ones instead of the hardware #DE behaviour of legacy x86. */
  if (!load(c, AX, SP, V, 32u, 0) || !load(c, DX, SP, V + 4, 32u, 0) ||
      !rr(c, 0x09u, AX, DX) || !test_reg(c, AX))
    return 0;
  size_t nonzero = jump(c, 5u);
  if (!division_trap(c) || !patch(c, nonzero, c->b.out->size))
    return 0;

  if (!imm(c, AX, 0u) || !store(c, AX, SP, R, 32u) ||
      !store(c, AX, SP, R + 4, 32u) || !store(c, AX, SP, Q, 32u) ||
      !store(c, AX, SP, Q + 4, 32u) || !imm(c, CX, 64u))
    return 0;

  size_t loop = c->b.out->size;
  /* Q <<= 1. */
  if (!load(c, AX, SP, Q, 32u, 0) || !load(c, DX, SP, Q + 4, 32u, 0) ||
      !double_shift_imm(c, 0, DX, AX, 1u) || !shift(c, 4u, AX, 1u) ||
      !store(c, AX, SP, Q, 32u) || !store(c, DX, SP, Q + 4, 32u))
    return 0;

  /* Carry the dividend's top bit into the low end of R while shifting D. */
  if (!load(c, AX, SP, D, 32u, 0) || !load(c, DX, SP, D + 4, 32u, 0) ||
      !shift(c, 4u, AX, 1u) || !shift(c, 2u, DX, 1u) ||
      !store(c, AX, SP, D, 32u) || !store(c, DX, SP, D + 4, 32u) ||
      !load(c, AX, SP, R, 32u, 0) || !load(c, DX, SP, R + 4, 32u, 0) ||
      !shift(c, 2u, AX, 1u) || !shift(c, 2u, DX, 1u) ||
      !store(c, AX, SP, R, 32u) || !store(c, DX, SP, R + 4, 32u))
    return 0;

  /* if (R >= V) { R -= V; Q |= 1; } -- unsigned comparison of pairs. */
  if (!load(c, AX, SP, R + 4, 32u, 0) || !load(c, DX, SP, V + 4, 32u, 0) ||
      !rr(c, 0x39u, AX, DX))
    return 0;
  size_t lower_hi = jump(c, 2u);   /* JB */
  size_t greater_hi = jump(c, 7u); /* JA */
  if (!load(c, AX, SP, R, 32u, 0) || !load(c, DX, SP, V, 32u, 0) ||
      !rr(c, 0x39u, AX, DX))
    return 0;
  size_t lower_lo = jump(c, 2u);
  if (!patch(c, greater_hi, c->b.out->size))
    return 0;
  if (!load(c, AX, SP, R, 32u, 0) || !load(c, DX, SP, V, 32u, 0) ||
      !rr(c, 0x29u, AX, DX) || !store(c, AX, SP, R, 32u) ||
      !load(c, AX, SP, R + 4, 32u, 0) || !load(c, DX, SP, V + 4, 32u, 0) ||
      !rr(c, 0x19u, AX, DX) || !store(c, AX, SP, R + 4, 32u) ||
      !load(c, AX, SP, Q, 32u, 0) || !group_imm(c, 1u, AX, 1u) ||
      !store(c, AX, SP, Q, 32u))
    return 0;
  if (!patch(c, lower_hi, c->b.out->size) ||
      !patch(c, lower_lo, c->b.out->size))
    return 0;

  if (!group_imm(c, 5u, CX, 1u))
    return 0;
  size_t again = jump(c, 5u);
  if (!patch(c, again, loop))
    return 0;

  if (sign) {
    /* Quotient sign is lhs XOR rhs; remainder sign follows lhs. */
    if (!load(c, AX, abase, ahome + 4, 32u, 0) ||
        !load(c, DX, bbase, bhome + 4, 32u, 0) || !rr(c, 0x31u, AX, DX) ||
        !test_reg(c, AX))
      return 0;
    size_t q_positive = jump(c, 9u);
    if (!negate64_mem(c, SP, Q) || !patch(c, q_positive, c->b.out->size))
      return 0;
    if (!load(c, AX, abase, ahome + 4, 32u, 0) || !test_reg(c, AX))
      return 0;
    size_t r_positive = jump(c, 9u);
    if (!negate64_mem(c, SP, R) || !patch(c, r_positive, c->b.out->size))
      return 0;
  }

  int32_t source = remainder ? R : Q;
  if (!copy(c, dstbase, dsthome, SP, source, 8u) ||
      !group_imm(c, 0u, SP, SCRATCH))
    return 0;
  return 1;
}

static int divide64(Legacy *c, const AstMirInstructionInfo *i, int sign) {
  AstMirVReg a = i->operands[0], b = i->operands[1];
  return divide64_mem(c, BP, c->homes[a], BP, c->homes[b], sign,
                      i->token_op == AST_TOK_PERCENT, 1, BP,
                      c->homes[i->result]);
}

static int mul64_wrap(Legacy *c, AstMirVReg a, AstMirVReg b, unsigned dstbase,
                      int32_t dsthome) {
  /* 64-bit product modulo 2^64, using only caller-saved registers.
   * a0*b0 contributes EDX:EAX; the two cross-products contribute only
   * their low 32 bits to the high half. */
  if (!get(c, a, AX, 0, 0) || !get(c, b, CX, 0, 0) || !unary_reg(c, 4, CX) ||
      !store(c, AX, dstbase, dsthome, 32u) ||
      !store(c, DX, dstbase, dsthome + 4, 32u) || !get(c, a, AX, 4, 0) ||
      !get(c, b, CX, 0, 0) || !prefix(c, 32, 0) || !byte(c, 0x0f) ||
      !byte(c, 0xaf) || !byte(c, 0xc1) ||
      !load(c, CX, dstbase, dsthome + 4, 32u, 0) || !rr(c, 0x01, AX, CX) ||
      !store(c, AX, dstbase, dsthome + 4, 32u) || !get(c, a, AX, 0, 0) ||
      !get(c, b, CX, 4, 0) || !prefix(c, 32, 0) || !byte(c, 0x0f) ||
      !byte(c, 0xaf) || !byte(c, 0xc1) ||
      !load(c, CX, dstbase, dsthome + 4, 32u, 0) || !rr(c, 0x01, AX, CX) ||
      !store(c, AX, dstbase, dsthome + 4, 32u))
    return 0;
  return 1;
}

static int binary(Legacy *c, const AstMirInstructionInfo *i) {
  AstMirVReg a = i->operands[0], b = i->operands[1];
  uint32_t size = value_size(c, a);
  int sign = ast_sema_type_is_signed(c->values[a].type);
  if (ast_sema_type_kind(c->values[a].type) == AST_SEMA_TYPE_FLOAT)
    return float_binary(c, i);
  if (condition(i->token_op, sign) >= 0)
    return compare(c, i);
  unsigned opcode =
      i->token_op == AST_TOK_PLUS                                       ? 0x01
      : i->token_op == AST_TOK_MINUS                                    ? 0x29
      : i->token_op == AST_TOK_AMP || i->token_op == AST_TOK_AMP_AMP    ? 0x21
      : i->token_op == AST_TOK_PIPE || i->token_op == AST_TOK_PIPE_PIPE ? 0x09
      : i->token_op == AST_TOK_CARET                                    ? 0x31
                                                                        : 0;
  if (opcode) {
    if (!get(c, a, AX, 0, 0) || !get(c, b, CX, 0, 0) ||
        !rr(c, opcode, AX, CX) || !put(c, i->result, AX, 0))
      return 0;
    if (size > 4) {
      if (opcode == 0x01)
        opcode = 0x11; /* ADC: loads/stores preserve CF. */
      if (opcode == 0x29)
        opcode = 0x19;
      if (!get(c, a, AX, 4, 0) || !get(c, b, CX, 4, 0) ||
          !rr(c, opcode, AX, CX) || !put(c, i->result, AX, 4))
        return 0;
    }
    return 1;
  }
  if (!get(c, a, AX, 0, sign) || !get(c, b, CX, 0, sign))
    return 0;
  if (i->token_op == AST_TOK_STAR) {
    if (size <= 4) {
      if (!prefix(c, 32, 0) || !byte(c, 0x0f) || !byte(c, 0xaf) ||
          !byte(c, 0xc1))
        return 0;
    } else {
      return mul64_wrap(c, a, b, BP, c->homes[i->result]);
    }
  } else if (i->token_op == AST_TOK_SLASH || i->token_op == AST_TOK_PERCENT) {
    if (size > 4)
      return divide64(c, i, sign);
    if (sign) {
      if (!prefix(c, 32, 0) || !byte(c, 0x99))
        return 0;
    } else if (!imm(c, DX, 0))
      return 0;
    if (!unary_reg(c, sign ? 7u : 6u, CX))
      return 0;
    if (i->token_op == AST_TOK_PERCENT && !rr(c, 0x89, AX, DX))
      return 0;
  } else if (i->token_op == AST_TOK_SHIFT_LEFT ||
             i->token_op == AST_TOK_SHIFT_RIGHT) {
    unsigned group = i->token_op == AST_TOK_SHIFT_LEFT ? 4u : sign ? 7u : 5u;
    if (size <= 4) {
      if (!shift_cl(c, group, AX))
        return 0;
    } else {
      /* AsterASM defines integer shifts modulo the value width.  IA-32
       * masks CL to 5 bits for 32-bit shifts, so a 64-bit value needs an
       * explicit split at count 32 after masking the source count to 63.
       *
       * count < 32:
       *   shl : EDX:EAX = SHLD(EDX,EAX,CL) : (EAX << CL)
       *   shr : EDX:EAX = (EDX >>/>>> CL) : SHRD(EAX,EDX,CL)
       * count >= 32:
       *   shl : EDX = EAX << (CL&31), EAX = 0
       *   lshr: EAX = EDX >> (CL&31), EDX = 0
       *   ashr: EAX = EDX >>> (CL&31), EDX = sign(EAX)
       */
      if (!group_imm(c, 4, CX, 63u) || !group_imm(c, 7, CX, 32u))
        return 0;
      size_t large = jump(c, 3); /* JAE: masked count >= 32 */
      if (!get(c, a, AX, 0, 0) || !get(c, a, DX, 4, 0))
        return 0;
      if (i->token_op == AST_TOK_SHIFT_LEFT) {
        if (!double_shift_cl(c, 0, DX, AX) || !shift_cl(c, 4u, AX))
          return 0;
      } else {
        if (!double_shift_cl(c, 1, AX, DX) || !shift_cl(c, sign ? 7u : 5u, DX))
          return 0;
      }
      size_t done = jump(c, -1);
      if (!patch(c, large, c->b.out->size))
        return 0;
      if (i->token_op == AST_TOK_SHIFT_LEFT) {
        if (!get(c, a, DX, 0, 0) || !shift_cl(c, 4u, DX) || !imm(c, AX, 0))
          return 0;
      } else {
        if (!get(c, a, AX, 4, 0))
          return 0;
        if (sign) {
          if (!rr(c, 0x89, DX, AX) || !shift(c, 7u, DX, 31u) ||
              !shift_cl(c, 7u, AX))
            return 0;
        } else if (!shift_cl(c, 5u, AX) || !imm(c, DX, 0))
          return 0;
      }
      if (!patch(c, done, c->b.out->size) || !put(c, i->result, AX, 0) ||
          !put(c, i->result, DX, 4))
        return 0;
      return 1;
    }
  } else
    return fail(c, "Unsupported legacy binary operation.");
  return put(c, i->result, AX, 0);
}
static int convert(Legacy *c, const AstMirInstructionInfo *i) {
  AstMirVReg src = i->operands[0];
  uint32_t from = value_size(c, src), to = value_size(c, i->result);
  AstSemaTypeKind from_kind = ast_sema_type_kind(c->values[src].type);
  AstSemaTypeKind to_kind = ast_sema_type_kind(i->type);
  if (i->token_op == AST_TOK_KW_BITCAST && from == to)
    return copy(c, BP, c->homes[i->result], BP, c->homes[src], to);
  if (to_kind == AST_SEMA_TYPE_FLOAT && from_kind == AST_SEMA_TYPE_FLOAT)
    return fp_load(c, src) && fp_store(c, i->result);
  if (to_kind == AST_SEMA_TYPE_FLOAT && from_kind != AST_SEMA_TYPE_FLOAT) {
    /* x87 FILD only accepts signed integer memory operands.  Normalize the
     * source into a signed 64-bit temporary first.  Unsigned u64 values
     * with the top bit set are interpreted by FILD as value-2^64, so add
     * the exactly representable 2^64 constant before storing f32/f64. */
    const int sign = ast_sema_type_is_signed(c->values[src].type);
    if (from > 8u || (to != 4u && to != 8u))
      return fail(c, "Legacy integer-to-float cast has no x87 representation.");
    if (!group_imm(c, 5u, SP, 16u) || !get(c, src, AX, 0u, sign) ||
        !store(c, AX, SP, 0, 32u))
      return 0;
    if (from > 4u) {
      if (!get(c, src, DX, 4u, 0) || !store(c, DX, SP, 4, 32u))
        return 0;
    } else if (sign) {
      if (!rr(c, 0x89u, DX, AX) || !shift(c, 7u, DX, 31u) ||
          !store(c, DX, SP, 4, 32u))
        return 0;
    } else if (!imm(c, DX, 0u) || !store(c, DX, SP, 4, 32u))
      return 0;

    if (!sign && from == 8u) {
      /* qword [sp+8] = 2^64 as IEEE-754 binary64. */
      if (!imm(c, AX, 0u) || !store(c, AX, SP, 8, 32u) ||
          !imm(c, AX, 0x43f00000u) || !store(c, AX, SP, 12, 32u) ||
          !load(c, DX, SP, 4, 32u, 0) || !test_reg(c, DX))
        return 0;
      size_t nonnegative = jump(c, 9u); /* JNS: source < 2^63. */
      if (!fp_fild64(c, SP, 0) || !fp_mem_base(c, 0xdcu, 0u, SP, 8))
        return 0; /* FADD m64real. */
      size_t done = jump(c, -1);
      if (!patch(c, nonnegative, c->b.out->size) || !fp_fild64(c, SP, 0) ||
          !patch(c, done, c->b.out->size))
        return 0;
    } else if (!fp_fild64(c, SP, 0))
      return 0;
    if (!fp_store(c, i->result) || !group_imm(c, 0u, SP, 16u))
      return 0;
    return 1;
  }
  if (from_kind == AST_SEMA_TYPE_FLOAT && to_kind != AST_SEMA_TYPE_FLOAT) {
    /* AsterASM numeric casts truncate toward zero.  FISTP follows the x87
     * control-word rounding mode, so save it, force RC=11 (truncate), and
     * restore it before leaving the conversion.  A u64 destination needs
     * one extra split at 2^63 because x87 has only signed 64-bit FISTP. */
    const int unsigned64 = !ast_sema_type_is_signed(i->type) && to == 8u;
    if (from != 4u && from != 8u)
      return fail(
          c, "Legacy float-to-integer cast has no x87 source representation.");
    if (to > 8u)
      return fail(c, "Legacy float-to-integer cast exceeds 64 bits.");
    if (!group_imm(c, 5u, SP, 24u) || !fp_fnstcw(c, SP, 8) ||
        !load(c, AX, SP, 8, 16u, 0) || !group_imm(c, 1u, AX, 0x0c00u) ||
        !store(c, AX, SP, 10, 16u) || !fp_fldcw(c, SP, 10))
      return 0;

    if (unsigned64) {
      /* qword [sp+16] = 2^63.  Compare a disposable load of the source
       * against it.  Values in [2^63,2^64) are converted after
       * subtracting 2^63, then the high bit is restored explicitly. */
      if (!imm(c, AX, 0u) || !store(c, AX, SP, 16, 32u) ||
          !imm(c, AX, 0x43e00000u) || !store(c, AX, SP, 20, 32u) ||
          !fp_load(c, src) || !fp_mem_base(c, 0xdcu, 3u, SP, 16) ||
          !byte(c, 0xdfu) || !byte(c, 0xe0u) || !byte(c, 0x9eu))
        return 0;
      size_t below = jump(c, 2u); /* JB: source < 2^63. */
      if (!fp_load(c, src) || !fp_mem_base(c, 0xdcu, 4u, SP, 16) ||
          !fp_fistp64(c, SP, 0) || !load(c, AX, SP, 4, 32u, 0) ||
          !group_imm(c, 1u, AX, 0x80000000u) || !store(c, AX, SP, 4, 32u))
        return 0;
      size_t converted = jump(c, -1);
      if (!patch(c, below, c->b.out->size) || !fp_load(c, src) ||
          !fp_fistp64(c, SP, 0) || !patch(c, converted, c->b.out->size))
        return 0;
    } else if (!fp_load(c, src) || !fp_fistp64(c, SP, 0))
      return 0;

    if (!fp_fldcw(c, SP, 8) || !copy(c, BP, c->homes[i->result], SP, 0, to) ||
        !group_imm(c, 0u, SP, 24u))
      return 0;
    return 1;
  }
  int sign = ast_sema_type_is_signed(c->values[src].type);
  if (i->token_op == AST_TOK_KW_ZEXT || i->token_op == AST_TOK_KW_BITCAST)
    sign = 0;
  if (!get(c, src, AX, 0, sign) || !put(c, i->result, AX, 0))
    return 0;
  if (to > 4) {
    if (from > 4) {
      if (!get(c, src, AX, 4, 0))
        return 0;
    } else if (sign) {
      if (!shift(c, 7, AX, 31))
        return 0;
    } else if (!imm(c, AX, 0))
      return 0;
    if (!put(c, i->result, AX, 4))
      return 0;
  }
  return 1;
}
static int intrinsic_clz_ctz(Legacy *c, const AstMirInstructionInfo *i,
                             int leading) {
  AstMirVReg v = i->operands[0];
  uint32_t size = value_size(c, v), width = c->values[v].bit_width;
  if (!width || width > 64u)
    return fail(c, "Legacy clz/ctz requires an integer width up to 64 bits.");
  if (size <= 4u) {
    if (!get(c, v, AX, 0, 0) || !test_reg(c, AX))
      return 0;
    size_t nonzero = jump(c, 5u);
    if (!imm(c, AX, width))
      return 0;
    size_t done = jump(c, -1);
    if (!patch(c, nonzero, c->b.out->size) || !bit_scan(c, leading, AX, AX))
      return 0;
    if (leading) {
      if (!imm(c, CX, width - 1u) || !rr(c, 0x29u, CX, AX) ||
          !rr(c, 0x89u, AX, CX))
        return 0;
    }
    if (!patch(c, done, c->b.out->size))
      return 0;
    return put(c, i->result, AX, 0);
  }

  if (leading) {
    if (!get(c, v, AX, 4u, 0) || !test_reg(c, AX))
      return 0;
    size_t high_zero = jump(c, 4u);
    if (!bit_scan(c, 1, AX, AX) || !imm(c, CX, 31u) || !rr(c, 0x29u, CX, AX) ||
        !rr(c, 0x89u, AX, CX))
      return 0;
    size_t done = jump(c, -1);
    if (!patch(c, high_zero, c->b.out->size) || !get(c, v, AX, 0u, 0) ||
        !test_reg(c, AX))
      return 0;
    size_t all_zero = jump(c, 4u);
    if (!bit_scan(c, 1, AX, AX) || !imm(c, CX, 63u) || !rr(c, 0x29u, CX, AX) ||
        !rr(c, 0x89u, AX, CX))
      return 0;
    size_t low_done = jump(c, -1);
    if (!patch(c, all_zero, c->b.out->size) || !imm(c, AX, 64u) ||
        !patch(c, low_done, c->b.out->size) || !patch(c, done, c->b.out->size))
      return 0;
  } else {
    if (!get(c, v, AX, 0u, 0) || !test_reg(c, AX))
      return 0;
    size_t low_zero = jump(c, 4u);
    if (!bit_scan(c, 0, AX, AX))
      return 0;
    size_t done = jump(c, -1);
    if (!patch(c, low_zero, c->b.out->size) || !get(c, v, AX, 4u, 0) ||
        !test_reg(c, AX))
      return 0;
    size_t all_zero = jump(c, 4u);
    if (!bit_scan(c, 0, AX, AX) || !group_imm(c, 0u, AX, 32u))
      return 0;
    size_t high_done = jump(c, -1);
    if (!patch(c, all_zero, c->b.out->size) || !imm(c, AX, 64u) ||
        !patch(c, high_done, c->b.out->size) || !patch(c, done, c->b.out->size))
      return 0;
  }
  if (!put(c, i->result, AX, 0) || !imm(c, AX, 0u) ||
      !put(c, i->result, AX, 4u))
    return 0;
  return 1;
}

static int intrinsic_popcnt(Legacy *c, const AstMirInstructionInfo *i) {
  AstMirVReg v = i->operands[0];
  uint32_t size = value_size(c, v);
  if (!imm(c, DX, 0u))
    return 0;
  for (uint32_t part = 0u; part < size; part += 4u) {
    if (!get(c, v, AX, part, 0))
      return 0;
    size_t loop = c->b.out->size;
    if (!test_reg(c, AX))
      return 0;
    size_t empty = jump(c, 4u);
    if (!rr(c, 0x89u, CX, AX) || !group_imm(c, 5u, CX, 1u) ||
        !rr(c, 0x21u, AX, CX) || !group_imm(c, 0u, DX, 1u))
      return 0;
    size_t again = jump(c, -1);
    if (!patch(c, again, loop) || !patch(c, empty, c->b.out->size))
      return 0;
  }
  if (!rr(c, 0x89u, AX, DX) || !put(c, i->result, AX, 0))
    return 0;
  if (size > 4u && (!imm(c, AX, 0u) || !put(c, i->result, AX, 4u)))
    return 0;
  return 1;
}

static int bswap32_reg(Legacy *c, unsigned reg, unsigned scratch) {
  /* 386-compatible byte swap: ROR16; split alternating bytes; shift and OR. */
  if (!rotate_imm_width(c, 1, reg, 32u, 16u) || !rr(c, 0x89u, scratch, reg) ||
      !group_imm(c, 4u, reg, 0x00ff00ffu) ||
      !group_imm(c, 4u, scratch, 0xff00ff00u) || !shift(c, 4u, reg, 8u) ||
      !shift(c, 5u, scratch, 8u) || !rr(c, 0x09u, reg, scratch))
    return 0;
  return 1;
}

static int intrinsic_bswap(Legacy *c, const AstMirInstructionInfo *i) {
  AstMirVReg v = i->operands[0];
  uint32_t size = value_size(c, v), width = c->values[v].bit_width;
  if (width == 8u)
    return copy(c, BP, c->homes[i->result], BP, c->homes[v], 1u);
  if (width == 16u) {
    if (!get(c, v, AX, 0u, 0) || !rotate_imm_width(c, 1, AX, 16u, 8u))
      return 0;
    return put(c, i->result, AX, 0u);
  }
  if (width == 32u) {
    if (!get(c, v, AX, 0u, 0) || !bswap32_reg(c, AX, DX))
      return 0;
    return put(c, i->result, AX, 0u);
  }
  if (width == 64u && size == 8u) {
    if (!get(c, v, AX, 0u, 0) || !bswap32_reg(c, AX, DX) ||
        !put(c, i->result, AX, 4u) || !get(c, v, AX, 4u, 0) ||
        !bswap32_reg(c, AX, DX) || !put(c, i->result, AX, 0u))
      return 0;
    return 1;
  }
  return fail(c, "Legacy bswap requires 8/16/32/64-bit integers.");
}

static int intrinsic_bitrev(Legacy *c, const AstMirInstructionInfo *i) {
  enum { D = 0, R = 8, SCRATCH = 16 };
  AstMirVReg v = i->operands[0];
  uint32_t size = value_size(c, v), width = c->values[v].bit_width;
  if (!width || width > 64u)
    return fail(c, "Legacy bitrev requires an integer width up to 64 bits.");
  if (!group_imm(c, 5u, SP, SCRATCH) || !imm(c, AX, 0u) ||
      !store(c, AX, SP, D, 32u) || !store(c, AX, SP, D + 4, 32u) ||
      !store(c, AX, SP, R, 32u) || !store(c, AX, SP, R + 4, 32u) ||
      !copy(c, SP, D, BP, c->homes[v], size) || !imm(c, CX, width))
    return 0;
  size_t loop = c->b.out->size;
  if (size <= 4u) {
    if (!load(c, AX, SP, R, 32u, 0) || !shift(c, 4u, AX, 1u) ||
        !store(c, AX, SP, R, 32u))
      return 0;
  } else {
    if (!load(c, AX, SP, R, 32u, 0) || !load(c, DX, SP, R + 4, 32u, 0) ||
        !double_shift_imm(c, 0, DX, AX, 1u) || !shift(c, 4u, AX, 1u) ||
        !store(c, AX, SP, R, 32u) || !store(c, DX, SP, R + 4, 32u))
      return 0;
  }
  if (!load(c, DX, SP, D, 32u, 0) || !group_imm(c, 4u, DX, 1u) ||
      !load(c, AX, SP, R, 32u, 0) || !rr(c, 0x09u, AX, DX) ||
      !store(c, AX, SP, R, 32u))
    return 0;
  if (size <= 4u) {
    if (!load(c, AX, SP, D, 32u, 0) || !shift(c, 5u, AX, 1u) ||
        !store(c, AX, SP, D, 32u))
      return 0;
  } else {
    if (!load(c, AX, SP, D, 32u, 0) || !load(c, DX, SP, D + 4, 32u, 0) ||
        !double_shift_imm(c, 1, AX, DX, 1u) || !shift(c, 5u, DX, 1u) ||
        !store(c, AX, SP, D, 32u) || !store(c, DX, SP, D + 4, 32u))
      return 0;
  }
  if (!group_imm(c, 5u, CX, 1u))
    return 0;
  size_t again = jump(c, 5u);
  if (!patch(c, again, loop) ||
      !copy(c, BP, c->homes[i->result], SP, R, size) ||
      !group_imm(c, 0u, SP, SCRATCH))
    return 0;
  return 1;
}

static int intrinsic_rotate(Legacy *c, const AstMirInstructionInfo *i,
                            int right) {
  AstMirVReg value = i->operands[0], count = i->operands[1];
  uint32_t size = value_size(c, value), width = c->values[value].bit_width;
  if (width <= 32u) {
    if (!get(c, value, AX, 0u, 0) || !get(c, count, CX, 0u, 0) ||
        !rotate_cl_width(c, right, AX, width))
      return 0;
    return put(c, i->result, AX, 0u);
  }
  if (width != 64u || size != 8u)
    return fail(c, "Legacy rotate requires 8/16/32/64-bit integers.");
  if (!get(c, count, CX, 0u, 0) || !group_imm(c, 4u, CX, 63u) ||
      !group_imm(c, 7u, CX, 32u))
    return 0;
  size_t large = jump(c, 3u);
  if (!get(c, value, AX, 0u, 0) || !get(c, value, DX, 4u, 0) ||
      !(right ? double_shift_cl(c, 1, AX, DX)
              : double_shift_cl(c, 0, AX, DX)) ||
      !put(c, i->result, AX, 0u) || !get(c, value, DX, 4u, 0) ||
      !get(c, value, AX, 0u, 0) ||
      !(right ? double_shift_cl(c, 1, DX, AX)
              : double_shift_cl(c, 0, DX, AX)) ||
      !put(c, i->result, DX, 4u))
    return 0;
  size_t done = jump(c, -1);
  if (!patch(c, large, c->b.out->size))
    return 0;
  /* count >= 32: the same SHLD/SHRD operation with swapped halves; CL already
   * contains count&31 as far as the instruction is concerned. */
  if (!get(c, value, AX, 4u, 0) || !get(c, value, DX, 0u, 0) ||
      !(right ? double_shift_cl(c, 1, AX, DX)
              : double_shift_cl(c, 0, AX, DX)) ||
      !put(c, i->result, AX, 0u) || !get(c, value, DX, 0u, 0) ||
      !get(c, value, AX, 4u, 0) ||
      !(right ? double_shift_cl(c, 1, DX, AX)
              : double_shift_cl(c, 0, DX, AX)) ||
      !put(c, i->result, DX, 4u) || !patch(c, done, c->b.out->size))
    return 0;
  return 1;
}

static int intrinsic_wrap_arith(Legacy *c, const AstMirInstructionInfo *i) {
  AstMirVReg a = i->operands[0], b = i->operands[1];
  uint32_t size = value_size(c, a);
  if (size > 4u) {
    if (i->intrinsic == AST_HIR_INTRINSIC_MUL_WRAP)
      return mul64_wrap(c, a, b, BP, c->homes[i->result]);
    unsigned opcode =
        i->intrinsic == AST_HIR_INTRINSIC_ADD_WRAP ? 0x01u : 0x29u;
    if (!get(c, a, AX, 0u, 0) || !get(c, b, CX, 0u, 0) ||
        !rr(c, opcode, AX, CX) || !put(c, i->result, AX, 0u))
      return 0;
    opcode = opcode == 0x01u ? 0x11u : 0x19u;
    if (!get(c, a, AX, 4u, 0) || !get(c, b, CX, 4u, 0) ||
        !rr(c, opcode, AX, CX) || !put(c, i->result, AX, 4u))
      return 0;
    return 1;
  }
  if (!get(c, a, AX, 0u, 0) || !get(c, b, CX, 0u, 0))
    return 0;
  if (i->intrinsic == AST_HIR_INTRINSIC_MUL_WRAP) {
    if (!prefix(c, 32, 0) || !byte(c, 0x0f) || !byte(c, 0xafu) ||
        !byte(c, 0xc1u))
      return 0;
  } else if (!rr(c, i->intrinsic == AST_HIR_INTRINSIC_ADD_WRAP ? 0x01u : 0x29u,
                 AX, CX))
    return 0;
  return put(c, i->result, AX, 0u);
}

static int intrinsic_checked_addsub(Legacy *c, const AstMirInstructionInfo *i,
                                    int subtract) {
  AstMirVReg a = i->operands[0], b = i->operands[1];
  uint32_t size = value_size(c, a), width = c->values[a].bit_width;
  int sign = ast_sema_type_is_signed(c->values[a].type);
  int32_t home = c->homes[i->result];
  if (size < 4u) {
    if (!get(c, a, AX, 0u, sign) || !get(c, b, CX, 0u, sign) ||
        !rr(c, subtract ? 0x29u : 0x01u, AX, CX) || !rr(c, 0x89u, DX, AX))
      return 0;
    if (sign) {
      uint32_t maxv = (1u << (width - 1u)) - 1u;
      uint32_t minv = (uint32_t)(-(int32_t)(1u << (width - 1u)));
      if (!group_imm(c, 7u, AX, maxv))
        return 0;
      size_t too_high = jump(c, 15u);
      if (!group_imm(c, 7u, AX, minv))
        return 0;
      size_t too_low = jump(c, 12u);
      if (!imm(c, AX, 0u))
        return 0;
      size_t done = jump(c, -1);
      if (!patch(c, too_high, c->b.out->size) ||
          !patch(c, too_low, c->b.out->size) || !imm(c, AX, 1u) ||
          !patch(c, done, c->b.out->size))
        return 0;
    } else if (subtract) {
      /* Recompute SUB so SETB sees the arithmetic CF; MOV above preserved it,
       * but the signed-only path may evolve independently. */
      if (!get(c, a, AX, 0u, 0) || !get(c, b, CX, 0u, 0) ||
          !rr(c, 0x29u, AX, CX) || !rr(c, 0x89u, DX, AX) || !setcc(c, 2u))
        return 0;
    } else {
      uint32_t maxv = (1u << width) - 1u;
      if (!rr(c, 0x89u, AX, DX) || !group_imm(c, 7u, AX, maxv) || !setcc(c, 7u))
        return 0;
    }
    return store(c, DX, BP, home, size_bits(size)) &&
           store_bool_home(c, home, size, AX);
  }
  if (size == 4u) {
    if (!get(c, a, AX, 0u, 0) || !get(c, b, CX, 0u, 0) ||
        !rr(c, subtract ? 0x29u : 0x01u, AX, CX) || !rr(c, 0x89u, DX, AX) ||
        !setcc(c, sign ? 0u : 2u))
      return 0;
    return store(c, DX, BP, home, 32u) && store_bool_home(c, home, size, AX);
  }
  if (size != 8u)
    return fail(c, "Legacy checked arithmetic requires <=64-bit integers.");
  if (!get(c, a, AX, 0u, 0) || !get(c, b, CX, 0u, 0) ||
      !rr(c, subtract ? 0x29u : 0x01u, AX, CX) ||
      !store(c, AX, BP, home, 32u) || !get(c, a, AX, 4u, 0) ||
      !get(c, b, CX, 4u, 0) || !rr(c, subtract ? 0x19u : 0x11u, AX, CX) ||
      !store(c, AX, BP, home + 4, 32u) || !setcc(c, sign ? 0u : 2u) ||
      !store_bool_home(c, home, size, AX))
    return 0;
  return 1;
}

static int intrinsic_checked_mul(Legacy *c, const AstMirInstructionInfo *i) {
  AstMirVReg a = i->operands[0], b = i->operands[1];
  uint32_t size = value_size(c, a), width = c->values[a].bit_width;
  int sign = ast_sema_type_is_signed(c->values[a].type);
  int32_t home = c->homes[i->result];
  if (size < 4u) {
    if (!get(c, a, AX, 0u, sign) || !get(c, b, CX, 0u, sign) ||
        !prefix(c, 32, 0) || !byte(c, 0x0f) || !byte(c, 0xafu) ||
        !byte(c, 0xc1u) || !rr(c, 0x89u, DX, AX))
      return 0;
    if (sign) {
      uint32_t maxv = (1u << (width - 1u)) - 1u;
      uint32_t minv = (uint32_t)(-(int32_t)(1u << (width - 1u)));
      if (!group_imm(c, 7u, AX, maxv))
        return 0;
      size_t too_high = jump(c, 15u);
      if (!group_imm(c, 7u, AX, minv))
        return 0;
      size_t too_low = jump(c, 12u);
      if (!imm(c, AX, 0u))
        return 0;
      size_t done = jump(c, -1);
      if (!patch(c, too_high, c->b.out->size) ||
          !patch(c, too_low, c->b.out->size) || !imm(c, AX, 1u) ||
          !patch(c, done, c->b.out->size))
        return 0;
    } else {
      uint32_t maxv = (1u << width) - 1u;
      if (!rr(c, 0x89u, AX, DX) || !group_imm(c, 7u, AX, maxv) || !setcc(c, 7u))
        return 0;
    }
    return store(c, DX, BP, home, size_bits(size)) &&
           store_bool_home(c, home, size, AX);
  }
  if (size == 4u) {
    if (!get(c, a, AX, 0u, 0) || !get(c, b, CX, 0u, 0) ||
        !unary_reg(c, sign ? 5u : 4u, CX) || !store(c, AX, BP, home, 32u) ||
        !setcc(c, sign ? 0u : 2u) || !store_bool_home(c, home, size, AX))
      return 0;
    return 1;
  }
  if (size != 8u)
    return fail(c, "Legacy checked multiplication requires <=64-bit integers.");
  if (!mul64_wrap(c, a, b, BP, home))
    return 0;
  /* Multiplication by zero never overflows. */
  if (!get(c, a, AX, 0u, 0) || !get(c, a, DX, 4u, 0) || !rr(c, 0x09u, AX, DX) ||
      !test_reg(c, AX))
    return 0;
  size_t nonzero = jump(c, 5u);
  if (!imm(c, AX, 0u) || !store_bool_home(c, home, size, AX))
    return 0;
  size_t done_zero = jump(c, -1);
  if (!patch(c, nonzero, c->b.out->size))
    return 0;

  size_t special_done = SIZE_MAX;
  if (sign) {
    /* wrapped INT64_MIN divided by -1 would #DE.  In the multiplication
     * check this exact state means overflow, so detect it before quotient. */
    if (!load(c, AX, BP, home + 4, 32u, 0) ||
        !group_imm(c, 7u, AX, 0x80000000u))
      return 0;
    size_t not_special0 = jump(c, 5u);
    if (!load(c, AX, BP, home, 32u, 0) || !group_imm(c, 7u, AX, 0u))
      return 0;
    size_t not_special1 = jump(c, 5u);
    if (!get(c, a, AX, 4u, 0) || !group_imm(c, 7u, AX, UINT32_MAX))
      return 0;
    size_t not_special2 = jump(c, 5u);
    if (!get(c, a, AX, 0u, 0) || !group_imm(c, 7u, AX, UINT32_MAX))
      return 0;
    size_t not_special3 = jump(c, 5u);
    if (!imm(c, AX, 1u) || !store_bool_home(c, home, size, AX))
      return 0;
    special_done = jump(c, -1);
    size_t normal = c->b.out->size;
    if (!patch(c, not_special0, normal) || !patch(c, not_special1, normal) ||
        !patch(c, not_special2, normal) || !patch(c, not_special3, normal))
      return 0;
  }

  /* Tuple padding after the bool is large enough for an 8-byte temporary
   * quotient when T=u64/i64: result occupies [0,8), bool [8,9), tuple size 16.
   * We may overwrite [8,16) here and write the final bool afterwards. */
  if (!divide64_mem(c, BP, home, BP, c->homes[a], sign, 0, 0, BP, home + 8))
    return 0;
  if (!load(c, AX, BP, home + 8, 32u, 0) || !get(c, b, DX, 0u, 0) ||
      !rr(c, 0x31u, AX, DX) || !rr(c, 0x89u, CX, AX) ||
      !load(c, AX, BP, home + 12, 32u, 0) || !get(c, b, DX, 4u, 0) ||
      !rr(c, 0x31u, AX, DX) || !rr(c, 0x09u, AX, CX) || !test_reg(c, AX) ||
      !setcc(c, 5u) || !store_bool_home(c, home, size, AX))
    return 0;
  if (special_done != SIZE_MAX && !patch(c, special_done, c->b.out->size))
    return 0;
  return patch(c, done_zero, c->b.out->size);
}

static int intrinsic_sat_addsub(Legacy *c, const AstMirInstructionInfo *i,
                                int subtract) {
  AstMirVReg a = i->operands[0], b = i->operands[1];
  uint32_t size = value_size(c, a), width = c->values[a].bit_width;
  int sign = ast_sema_type_is_signed(c->values[a].type);
  int32_t home = c->homes[i->result];
  if (size < 4u) {
    if (!get(c, a, AX, 0u, sign) || !get(c, b, CX, 0u, sign) ||
        !rr(c, subtract ? 0x29u : 0x01u, AX, CX))
      return 0;
    if (sign) {
      uint32_t maxv = (1u << (width - 1u)) - 1u;
      uint32_t minv = (uint32_t)(-(int32_t)(1u << (width - 1u)));
      if (!group_imm(c, 7u, AX, maxv))
        return 0;
      size_t high = jump(c, 15u);
      if (!group_imm(c, 7u, AX, minv))
        return 0;
      size_t low = jump(c, 12u);
      size_t done = jump(c, -1);
      if (!patch(c, high, c->b.out->size) || !imm(c, AX, maxv))
        return 0;
      size_t high_done = jump(c, -1);
      if (!patch(c, low, c->b.out->size) || !imm(c, AX, minv) ||
          !patch(c, done, c->b.out->size) ||
          !patch(c, high_done, c->b.out->size))
        return 0;
    } else if (subtract) {
      size_t no_underflow = jump(c, 3u); /* JAE = !CF, immediately after SUB. */
      if (!imm(c, AX, 0u) || !patch(c, no_underflow, c->b.out->size))
        return 0;
    } else {
      uint32_t maxv = (1u << width) - 1u;
      if (!group_imm(c, 7u, AX, maxv))
        return 0;
      size_t in_range = jump(c, 6u); /* JBE */
      if (!imm(c, AX, maxv) || !patch(c, in_range, c->b.out->size))
        return 0;
    }
    return store(c, AX, BP, home, size_bits(size));
  }
  if (size == 4u) {
    if (!get(c, a, AX, 0u, 0) || !get(c, b, CX, 0u, 0) ||
        !rr(c, subtract ? 0x29u : 0x01u, AX, CX))
      return 0;
    size_t in_range = jump(c, sign ? 1u : 3u); /* JNO or JAE */
    if (!sign) {
      if (!imm(c, AX, subtract ? 0u : UINT32_MAX))
        return 0;
    } else {
      if (!get(c, a, CX, 0u, 0) || !test_reg(c, CX))
        return 0;
      size_t positive = jump(c, 9u);
      if (!imm(c, AX, 0x80000000u))
        return 0;
      size_t saturated = jump(c, -1);
      if (!patch(c, positive, c->b.out->size) || !imm(c, AX, 0x7fffffffu) ||
          !patch(c, saturated, c->b.out->size))
        return 0;
    }
    if (!patch(c, in_range, c->b.out->size))
      return 0;
    return store(c, AX, BP, home, 32u);
  }
  if (size != 8u)
    return fail(c, "Legacy saturating arithmetic requires <=64-bit integers.");
  if (!get(c, a, AX, 0u, 0) || !get(c, b, CX, 0u, 0) ||
      !rr(c, subtract ? 0x29u : 0x01u, AX, CX) ||
      !store(c, AX, BP, home, 32u) || !get(c, a, AX, 4u, 0) ||
      !get(c, b, CX, 4u, 0) || !rr(c, subtract ? 0x19u : 0x11u, AX, CX) ||
      !store(c, AX, BP, home + 4, 32u))
    return 0;
  size_t in_range = jump(c, sign ? 1u : 3u);
  if (!sign) {
    if (!imm(c, AX, subtract ? 0u : UINT32_MAX) ||
        !store(c, AX, BP, home, 32u) || !store(c, AX, BP, home + 4, 32u))
      return 0;
  } else {
    if (!get(c, a, CX, 4u, 0) || !test_reg(c, CX))
      return 0;
    size_t positive = jump(c, 9u);
    if (!imm(c, AX, 0u) || !store(c, AX, BP, home, 32u) ||
        !imm(c, AX, 0x80000000u) || !store(c, AX, BP, home + 4, 32u))
      return 0;
    size_t saturated = jump(c, -1);
    if (!patch(c, positive, c->b.out->size) || !imm(c, AX, UINT32_MAX) ||
        !store(c, AX, BP, home, 32u) || !imm(c, AX, 0x7fffffffu) ||
        !store(c, AX, BP, home + 4, 32u) ||
        !patch(c, saturated, c->b.out->size))
      return 0;
  }
  return patch(c, in_range, c->b.out->size);
}

static int aggregate_type(const AstSemaType *type) {
  if (!type)
    return 0;
  AstSemaTypeKind k = ast_sema_type_kind(type);
  return k == AST_SEMA_TYPE_STRUCT || k == AST_SEMA_TYPE_ARRAY ||
         k == AST_SEMA_TYPE_TUPLE;
}

static int call(Legacy *c, const AstMirInstructionInfo *i) {
  uint32_t first = i->op == AST_MIR_OP_CALL_INDIRECT ? 1u : 0u, bytes = 0;
  const int sret = i->result != AST_MIR_INVALID_ID && aggregate_type(i->type);
  if (sret)
    bytes = 4u; /* hidden return-buffer pointer */
  for (uint32_t n = first; n < i->operand_count; ++n) {
    uint32_t size = value_size(c, i->operands[n]);
    if (size > UINT32_MAX - bytes - 3)
      return fail(c, "Legacy call argument area overflow.");
    bytes += (size + 3u) & ~3u;
  }
  uint32_t reserve = (bytes + 15u) & ~15u;
  if (reserve && !group_imm(c, 5, SP, reserve))
    return 0;
  uint32_t at = 0;
  if (sret) {
    if (!lea(c, AX, BP, c->homes[i->result]) || !store(c, AX, SP, 0, 32))
      return 0;
    at = 4u;
  }
  for (uint32_t n = first; n < i->operand_count; ++n) {
    AstMirVReg v = i->operands[n];
    uint32_t size = value_size(c, v);
    if (!copy(c, SP, (int32_t)at, BP, c->homes[v], size))
      return 0;
    at += (size + 3u) & ~3u;
  }
  if (first) {
    if (!get(c, i->operands[0], AX, 0, 0) || !prefix(c, 32, 0) ||
        !byte(c, 0xff) || !byte(c, 0xd0))
      return 0;
  } else if (!prefix(c, 32, 0) || !byte(c, 0xe8) || !word32(c, 0) ||
             !encoder_fixup(&c->b, c->b.out->size - 4, 4,
                            AST_ENCODER_FIXUP_X86_CALL_REL32, i->module,
                            i->symbol))
    return 0;
  /* SysV i386 aggregate-return callees remove the hidden pointer with RET 4.
   * Source arguments remain caller-cleaned. */
  uint32_t cleanup = reserve - (sret ? 4u : 0u);
  if (cleanup && !group_imm(c, 0, SP, cleanup))
    return 0;
  if (i->result == AST_MIR_INVALID_ID || sret)
    return 1;
  if (ast_sema_type_kind(i->type) == AST_SEMA_TYPE_FLOAT)
    return fp_store(c, i->result);
  if (value_size(c, i->result) > 8)
    return fail(
        c, "Legacy scalar/aggregate return has no direct register contract.");
  return put(c, i->result, AX, 0) && put(c, i->result, DX, 4);
}

static int legacy_name_eq(AstName name, const char *text) {
  size_t n = text ? strlen(text) : 0u;
  return text && name.text && (size_t)name.span.length == n &&
         memcmp(name.text, text, n) == 0;
}

static int legacy_atomic_kind(AstName name) {
  static const char *const names[] = {"load",    "store",   "rmw.xchg",
                                      "rmw.add", "rmw.sub", "rmw.and",
                                      "rmw.or",  "rmw.xor", "cas"};
  for (int op = AT_LOAD; op <= AT_CAS; ++op)
    if (legacy_name_eq(name, names[op]))
      return op;
  return -1;
}

static int legacy_has_feature(Legacy *c, uint64_t feature) {
  uint64_t have =
      ast_target_feature_closure(c->b.target.arch, c->b.target.features);
  return (have & feature) == feature;
}

/* r32 <- r/m32 arithmetic.  Keeping the source in its stable EBP home lets
 * CMPXCHG8B retain EDX:EAX as the compare value and ECX:EBX as the candidate.
 */
static int legacy_reg_mem32(Legacy *c, unsigned opcode, unsigned reg,
                            unsigned base, int32_t off) {
  return prefix(c, 32u, 1) && byte(c, opcode) && mem_tail(c, reg, base, off);
}

static int legacy_cmpxchg(Legacy *c, unsigned base, unsigned desired,
                          unsigned bits) {
  return byte(c, 0xf0u) && prefix(c, bits, 1) && byte(c, 0x0fu) &&
         byte(c, bits == 8u ? 0xb0u : 0xb1u) && mem_tail(c, desired, base, 0);
}

static int legacy_cmpxchg8b(Legacy *c, unsigned base) {
  /* CMPXCHG8B has a fixed 64-bit operand independent of the code-segment
   * default operand size.  code16 only needs the 32-bit address-size prefix. */
  return byte(c, 0xf0u) && (!c->mode16 || byte(c, 0x67u)) && byte(c, 0x0fu) &&
         byte(c, 0xc7u) && mem_tail(c, 1u, base, 0);
}

static int legacy_atomic_64(Legacy *c, const AstMirInstructionInfo *i, int op) {
  if (!legacy_has_feature(c, AST_TARGET_FEATURE_X86_CMPXCHG8B))
    return fail(
        c, "64-bit legacy atomics require the i586/cmpxchg8b target feature.");
  if (i->operand_count < 1u || i->operands[0] == AST_MIR_INVALID_ID)
    return 0;

  /* EBX and ESI are cdecl callee-saved.  The rest of the fixed CMPXCHG8B
   * bundle (EAX, EDX, ECX) is caller-saved. */
  if (!push(c, BX) || !push(c, SI) || !get(c, i->operands[0], SI, 0u, 0))
    return 0;

  int ok = 1;
  if (op == AT_CAS) {
    if (i->operand_count != 3u || i->result == AST_MIR_INVALID_ID)
      ok = 0;
    if (ok)
      ok = get(c, i->operands[1], AX, 0u, 0) &&
           get(c, i->operands[1], DX, 4u, 0) &&
           get(c, i->operands[2], BX, 0u, 0) &&
           get(c, i->operands[2], CX, 4u, 0) && legacy_cmpxchg8b(c, SI) &&
           setcc(c, 4u);
  } else if (op == AT_LOAD) {
    if (i->operand_count != 1u || i->result == AST_MIR_INVALID_ID)
      ok = 0;
    if (ok)
      ok = imm(c, AX, 0u) && imm(c, DX, 0u) && imm(c, BX, 0u) &&
           imm(c, CX, 0u) && legacy_cmpxchg8b(c, SI);
  } else if (op == AT_STORE || op == AT_XCHG) {
    if (i->operand_count != 2u ||
        (op == AT_XCHG && i->result == AST_MIR_INVALID_ID))
      ok = 0;
    if (ok)
      ok = get(c, i->operands[1], BX, 0u, 0) &&
           get(c, i->operands[1], CX, 4u, 0) && imm(c, AX, 0u) &&
           imm(c, DX, 0u);
    size_t retry = c->b.out->size;
    if (ok)
      ok = legacy_cmpxchg8b(c, SI);
    if (ok) {
      size_t jne = jump(c, 5u);
      ok = patch(c, jne, retry);
    }
  } else {
    if (i->operand_count != 2u || i->result == AST_MIR_INVALID_ID)
      ok = 0;
    /* Obtain one coherent initial expected value without changing memory:
     * compare 0 and, on success, write the same 0.  On failure the CPU
     * atomically publishes memory into EDX:EAX. */
    if (ok)
      ok = imm(c, AX, 0u) && imm(c, DX, 0u) && imm(c, BX, 0u) &&
           imm(c, CX, 0u) && legacy_cmpxchg8b(c, SI);
    size_t retry = c->b.out->size;
    if (ok)
      ok = rr(c, 0x89u, BX, AX) && rr(c, 0x89u, CX, DX);
    if (ok) {
      int32_t home = c->homes[i->operands[1]];
      if (op == AT_ADD)
        ok = legacy_reg_mem32(c, 0x03u, BX, BP, home) &&
             legacy_reg_mem32(c, 0x13u, CX, BP, home + 4);
      else if (op == AT_SUB)
        ok = legacy_reg_mem32(c, 0x2bu, BX, BP, home) &&
             legacy_reg_mem32(c, 0x1bu, CX, BP, home + 4);
      else {
        unsigned opc = op == AT_AND   ? 0x23u
                       : op == AT_OR  ? 0x0bu
                       : op == AT_XOR ? 0x33u
                                      : 0u;
        ok = opc && legacy_reg_mem32(c, opc, BX, BP, home) &&
             legacy_reg_mem32(c, opc, CX, BP, home + 4);
      }
    }
    if (ok)
      ok = legacy_cmpxchg8b(c, SI);
    if (ok) {
      size_t jne = jump(c, 5u);
      ok = patch(c, jne, retry);
    }
  }

  if (ok && i->result != AST_MIR_INVALID_ID) {
    if (op == AT_CAS)
      ok = put(c, i->result, AX, 0u);
    else
      ok = put(c, i->result, AX, 0u) && put(c, i->result, DX, 4u);
  }
  return ok && pop(c, SI) && pop(c, BX);
}

static int legacy_atomic(Legacy *c, const AstMirInstructionInfo *i) {
  int op = legacy_atomic_kind(i->symbol);
  if (op < 0 || i->operand_count < 1u)
    return fail(c, "Unknown legacy atomic operation.");
  uint32_t bits = 0u;
  if (op == AT_LOAD)
    bits = i->result == AST_MIR_INVALID_ID ? 0u : value_size(c, i->result) * 8u;
  else if (i->operand_count > 1u)
    bits = value_size(c, i->operands[1]) * 8u;
  if (bits == 1u)
    bits = 8u;
  if (bits == 64u)
    return legacy_atomic_64(c, i, op);
  if (bits != 8u && bits != 16u && bits != 32u)
    return fail(c, "Legacy atomic storage must be 8/16/32/64 bits.");

  if (op == AT_LOAD) {
    return i->operand_count == 1u && i->result != AST_MIR_INVALID_ID &&
           get(c, i->operands[0], CX, 0u, 0) && load(c, AX, CX, 0, bits, 0) &&
           put(c, i->result, AX, 0u);
  }
  if (op == AT_STORE) {
    if (i->operand_count != 2u || !get(c, i->operands[0], CX, 0u, 0) ||
        !get(c, i->operands[1], AX, 0u, 0))
      return 0;
    if (i->memory_order == AST_MEMORY_ORDER_SEQ_CST)
      return prefix(c, bits, 1) && byte(c, bits == 8u ? 0x86u : 0x87u) &&
             mem_tail(c, AX, CX, 0);
    return store(c, AX, CX, 0, bits);
  }
  if (op == AT_XCHG) {
    return i->operand_count == 2u && i->result != AST_MIR_INVALID_ID &&
           get(c, i->operands[0], CX, 0u, 0) &&
           get(c, i->operands[1], AX, 0u, 0) && prefix(c, bits, 1) &&
           byte(c, bits == 8u ? 0x86u : 0x87u) && mem_tail(c, AX, CX, 0) &&
           put(c, i->result, AX, 0u);
  }
  if (!legacy_has_feature(c, AST_TARGET_FEATURE_X86_CMPXCHG))
    return fail(c, "Legacy CAS/RMW requires the i486/cmpxchg target feature.");
  if (op == AT_CAS) {
    return i->operand_count == 3u && i->result != AST_MIR_INVALID_ID &&
           get(c, i->operands[0], CX, 0u, 0) &&
           get(c, i->operands[1], AX, 0u, 0) &&
           get(c, i->operands[2], DX, 0u, 0) &&
           legacy_cmpxchg(c, CX, DX, bits) && setcc(c, 4u) &&
           put(c, i->result, AX, 0u);
  }

  /* General RMW loop.  Preserve ESI/EBX because they are cdecl callee-saved;
   * their byte/word aliases also give us a desired register for 8-bit CAS. */
  if (i->operand_count != 2u || i->result == AST_MIR_INVALID_ID ||
      !push(c, BX) || !push(c, SI) || !get(c, i->operands[0], SI, 0u, 0) ||
      !get(c, i->operands[1], BX, 0u, 0) || !load(c, AX, SI, 0, bits, 0))
    return 0;
  size_t retry = c->b.out->size;
  if (!rr(c, 0x89u, DX, AX))
    return 0;
  unsigned opc = op == AT_ADD   ? 0x01u
                 : op == AT_SUB ? 0x29u
                 : op == AT_AND ? 0x21u
                 : op == AT_OR  ? 0x09u
                 : op == AT_XOR ? 0x31u
                                : 0u;
  if (!opc || !rr(c, opc, DX, BX) || !legacy_cmpxchg(c, SI, DX, bits))
    return 0;
  size_t jne = jump(c, 5u);
  if (!patch(c, jne, retry) || !put(c, i->result, AX, 0u))
    return 0;
  return pop(c, SI) && pop(c, BX);
}

static int instruction(Legacy *c, const AstMirInstructionInfo *i) {
  AstMirVReg result = i->result;
  uint32_t size = result == AST_MIR_INVALID_ID ? 0 : value_size(c, result);
  switch (i->op) {
  case AST_MIR_OP_CONSTANT: {
    uint64_t bits = i->constant.lo;
    if (i->constant.kind == AST_SEMA_CONST_FLOAT) {
      if (size == 4) {
        float v = (float)i->constant.floating;
        uint32_t u;
        memcpy(&u, &v, 4);
        bits = u;
      } else
        memcpy(&bits, &i->constant.floating, 8);
    } else if (i->constant.is_negative)
      bits = ~bits + 1;
    for (uint32_t off = 0; off < size; off += 4)
      if (!imm(c, AX, (uint32_t)(bits >> (8u * off))) ||
          !put(c, result, AX, off))
        return 0;
    return 1;
  }
  case AST_MIR_OP_UNDEF:
    if (!imm(c, AX, 0))
      return 0;
    for (uint32_t off = 0; off < size; off += 4)
      if (!put(c, result, AX, off))
        return 0;
    return 1;
  case AST_MIR_OP_COPY:
  case AST_MIR_OP_BIND:
    return copy(c, BP, c->homes[result], BP, c->homes[i->operands[0]], size);
  case AST_MIR_OP_STACK_ADDR:
    return lea(c, AX, BP, c->slots[i->stack_slot]) && put(c, result, AX, 0);
  case AST_MIR_OP_STACK_LOAD:
    return copy(c, BP, c->homes[result], BP, c->slots[i->stack_slot], size);
  case AST_MIR_OP_STACK_STORE:
    return copy(c, BP, c->slots[i->stack_slot], BP, c->homes[i->operands[0]],
                value_size(c, i->operands[0]));
  case AST_MIR_OP_GLOBAL_ADDR:
  case AST_MIR_OP_FUNCTION_ADDR:
    return symbol(c, AX, i->module, i->symbol) && put(c, result, AX, 0);
  case AST_MIR_OP_GLOBAL_LOAD:
    return symbol(c, CX, i->module, i->symbol) &&
           copy(c, BP, c->homes[result], CX, 0, size);
  case AST_MIR_OP_GLOBAL_STORE:
    return symbol(c, CX, i->module, i->symbol) &&
           copy(c, CX, 0, BP, c->homes[i->operands[0]],
                value_size(c, i->operands[0]));
  case AST_MIR_OP_LOAD:
    return get(c, i->operands[0], CX, 0, 0) &&
           copy(c, BP, c->homes[result], CX, 0, size);
  case AST_MIR_OP_STORE:
    return get(c, i->operands[0], CX, 0, 0) &&
           copy(c, CX, 0, BP, c->homes[i->operands[1]],
                value_size(c, i->operands[1]));
  case AST_MIR_OP_BINARY:
    return binary(c, i);
  case AST_MIR_OP_CAST:
    return convert(c, i);
  case AST_MIR_OP_CALL_DIRECT:
  case AST_MIR_OP_CALL_INDIRECT:
    return call(c, i);
  case AST_MIR_OP_UNARY:
    if (ast_sema_type_kind(i->type) == AST_SEMA_TYPE_FLOAT) {
      if (i->token_op != AST_TOK_MINUS && i->token_op != AST_TOK_PLUS)
        return fail(c, "Unsupported legacy floating unary operation.");
      return fp_load(c, i->operands[0]) &&
             (i->token_op != AST_TOK_MINUS ||
              (byte(c, 0xd9) && byte(c, 0xe0))) &&
             fp_store(c, result);
    }
    if (!get(c, i->operands[0], AX, 0, 0))
      return 0;
    if (i->token_op == AST_TOK_BANG) {
      if (value_size(c, i->operands[0]) > 4 &&
          (!get(c, i->operands[0], CX, 4, 0) || !rr(c, 0x09, AX, CX)))
        return 0;
      if (!rr(c, 0x85, AX, AX) || !setcc(c, 4))
        return 0;
    } else if (i->token_op == AST_TOK_MINUS || i->token_op == AST_TOK_TILDE) {
      if (!unary_reg(c, i->token_op == AST_TOK_MINUS ? 3u : 2u, AX))
        return 0;
    } else if (i->token_op != AST_TOK_PLUS)
      return fail(c, "Unsupported legacy unary operation.");
    if (!put(c, result, AX, 0))
      return 0;
    if (size > 4) {
      if (!get(c, i->operands[0], AX, 4, 0))
        return 0;
      if (i->token_op == AST_TOK_MINUS &&
          (!group_imm(c, 2, AX, 0) || !unary_reg(c, 3, AX)))
        return 0;
      if (i->token_op == AST_TOK_TILDE && !unary_reg(c, 2, AX))
        return 0;
      if (!put(c, result, AX, 4))
        return 0;
    }
    return 1;
  case AST_MIR_OP_SELECT: {
    if (size <= 8u)
      return scalar_select(c, result, i->operands[0], i->operands[1],
                           i->operands[2]);
    if (!get(c, i->operands[0], AX, 0, 0) || !rr(c, 0x85, AX, AX))
      return 0;
    size_t otherwise = jump(c, 4);
    if (!copy(c, BP, c->homes[result], BP, c->homes[i->operands[1]], size))
      return 0;
    size_t done = jump(c, -1);
    return patch(c, otherwise, c->b.out->size) &&
           copy(c, BP, c->homes[result], BP, c->homes[i->operands[2]], size) &&
           patch(c, done, c->b.out->size);
  }
  case AST_MIR_OP_PACK:
    return aggregate_pack(c, i);
  case AST_MIR_OP_INDEX:
    return aggregate_extract(c, i, 0);
  case AST_MIR_OP_MEMBER:
    return aggregate_extract(c, i, 1);
  case AST_MIR_OP_ATOMIC:
    return legacy_atomic(c, i);
  case AST_MIR_OP_FENCE:
    return legacy_fence(c, i->memory_order);
  case AST_MIR_OP_INTRINSIC:
    if (i->intrinsic == AST_HIR_INTRINSIC_SELECT) {
      if (i->operand_count != 3u || result == AST_MIR_INVALID_ID)
        return 0;
      if (size <= 8u)
        return scalar_select(c, result, i->operands[0], i->operands[1],
                             i->operands[2]);
      if (!get(c, i->operands[0], AX, 0, 0) || !rr(c, 0x85, AX, AX))
        return 0;
      size_t otherwise = jump(c, 4);
      if (!copy(c, BP, c->homes[result], BP, c->homes[i->operands[1]], size))
        return 0;
      size_t done = jump(c, -1);
      return patch(c, otherwise, c->b.out->size) &&
             copy(c, BP, c->homes[result], BP, c->homes[i->operands[2]],
                  size) &&
             patch(c, done, c->b.out->size);
    }
    if (i->intrinsic == AST_HIR_INTRINSIC_PTR_TO_INT ||
        i->intrinsic == AST_HIR_INTRINSIC_INT_TO_PTR)
      return convert(c, i);
    if (i->intrinsic == AST_HIR_INTRINSIC_CLZ)
      return intrinsic_clz_ctz(c, i, 1);
    if (i->intrinsic == AST_HIR_INTRINSIC_CTZ)
      return intrinsic_clz_ctz(c, i, 0);
    if (i->intrinsic == AST_HIR_INTRINSIC_POPCNT)
      return intrinsic_popcnt(c, i);
    if (i->intrinsic == AST_HIR_INTRINSIC_BSWAP)
      return intrinsic_bswap(c, i);
    if (i->intrinsic == AST_HIR_INTRINSIC_BITREV)
      return intrinsic_bitrev(c, i);
    if (i->intrinsic == AST_HIR_INTRINSIC_ROL)
      return intrinsic_rotate(c, i, 0);
    if (i->intrinsic == AST_HIR_INTRINSIC_ROR)
      return intrinsic_rotate(c, i, 1);
    if (i->intrinsic == AST_HIR_INTRINSIC_ADD_WRAP ||
        i->intrinsic == AST_HIR_INTRINSIC_SUB_WRAP ||
        i->intrinsic == AST_HIR_INTRINSIC_MUL_WRAP)
      return intrinsic_wrap_arith(c, i);
    if (i->intrinsic == AST_HIR_INTRINSIC_ADD_CHECKED)
      return intrinsic_checked_addsub(c, i, 0);
    if (i->intrinsic == AST_HIR_INTRINSIC_SUB_CHECKED)
      return intrinsic_checked_addsub(c, i, 1);
    if (i->intrinsic == AST_HIR_INTRINSIC_MUL_CHECKED)
      return intrinsic_checked_mul(c, i);
    if (i->intrinsic == AST_HIR_INTRINSIC_ADD_SAT)
      return intrinsic_sat_addsub(c, i, 0);
    if (i->intrinsic == AST_HIR_INTRINSIC_SUB_SAT)
      return intrinsic_sat_addsub(c, i, 1);
    return fail(c, "Unsupported legacy intrinsic.");
  default:
    return fail(c, "MIR operation has no legacy x86 lowering.");
  }
}
static int edge(Legacy *c, uint32_t target, const AstMirVReg *args,
                uint32_t count, int allow_fallthrough) {
  AstMirBlockInfo block;
  if (!ast_mir_function_block(c->fn, target, &block) ||
      block.argument_count != count)
    return fail(c, "Invalid legacy CFG edge.");
  uint32_t at = 0;
  for (uint32_t n = 0; n < count; ++n) {
    uint32_t size = value_size(c, args[n]);
    if (!copy(c, BP, c->edge_home + (int32_t)at, BP, c->homes[args[n]], size))
      return 0;
    at += (size + 3u) & ~3u;
  }
  at = 0;
  for (uint32_t n = 0; n < count; ++n) {
    uint32_t size = value_size(c, block.arguments[n]);
    if (!copy(c, BP, c->homes[block.arguments[n]], BP,
              c->edge_home + (int32_t)at, size))
      return 0;
    at += (size + 3u) & ~3u;
  }
  /* A branch to the immediately following emitted block is a true fallthrough.
   * Omitting it in both layout passes removes the legacy `jmp +0` artifact
   * without changing block offsets or relocation positions. */
  if (allow_fallthrough && target == c->next_block)
    return 1;
  size_t at_jump = jump(c, -1);
  return patch(c, at_jump, c->blocks[target]);
}
static int epilogue(Legacy *c, uint16_t pop_bytes) {
  if (!rr(c, 0x89, SP, BP) || !pop(c, BP) || !prefix(c, 32, 0))
    return 0;
  if (!pop_bytes)
    return byte(c, 0xc3);
  return byte(c, 0xc2) && byte(c, pop_bytes & 0xffu) &&
         byte(c, pop_bytes >> 8u);
}
static int terminator(Legacy *c, const AstMirTerminatorInfo *t) {
  if (t->kind == AST_MIR_TERM_RETURN) {
    if (c->has_sret) {
      if (t->condition_or_value == AST_MIR_INVALID_ID)
        return fail(c, "Legacy aggregate return is missing its value.");
      AstMirVReg v = t->condition_or_value;
      if (!load(c, CX, BP, 8, 32, 0) ||
          !copy(c, CX, 0, BP, c->homes[v], c->sret_size) ||
          !load(c, AX, BP, 8, 32, 0))
        return 0;
      return epilogue(c, 4u);
    }
    if (t->condition_or_value != AST_MIR_INVALID_ID) {
      AstMirVReg v = t->condition_or_value;
      if (ast_sema_type_kind(c->values[v].type) == AST_SEMA_TYPE_FLOAT) {
        if (!fp_load(c, v))
          return 0;
      } else if (!get(c, v, AX, 0, 0) || !get(c, v, DX, 4, 0))
        return 0;
    }
    return epilogue(c, 0u);
  }
  if (t->kind == AST_MIR_TERM_UNREACHABLE)
    return byte(c, 0x0f) && byte(c, 0x0b);
  if (t->kind == AST_MIR_TERM_BR)
    return edge(c, t->target_true, t->true_args, t->true_arg_count, 1);
  if (t->kind == AST_MIR_TERM_COND_BR) {
    if (!get(c, t->condition_or_value, AX, 0, 0) || !rr(c, 0x85, AX, AX))
      return 0;
    if (!t->true_arg_count && !t->false_arg_count &&
        (t->target_true == c->next_block || t->target_false == c->next_block)) {
      uint32_t target =
          t->target_true == c->next_block ? t->target_false : t->target_true;
      size_t branch = jump(c, t->target_true == c->next_block ? 4 : 5);
      return patch(c, branch, c->blocks[target]);
    }
    if (t->target_true == c->next_block) {
      size_t yes = jump(c, 5);
      return edge(c, t->target_false, t->false_args, t->false_arg_count, 0) &&
             patch(c, yes, c->b.out->size) &&
             edge(c, t->target_true, t->true_args, t->true_arg_count, 1);
    }
    size_t otherwise = jump(c, 4);
    return edge(c, t->target_true, t->true_args, t->true_arg_count, 0) &&
           patch(c, otherwise, c->b.out->size) &&
           edge(c, t->target_false, t->false_args, t->false_arg_count, 1);
  }
  return fail(c, "Missing legacy terminator.");
}

static int dce_candidate(const AstMirInstructionInfo *i) {
  const uint32_t effects = AST_MIR_INST_READS_MEMORY |
                           AST_MIR_INST_WRITES_MEMORY | AST_MIR_INST_ATOMIC |
                           AST_MIR_INST_CALL | AST_MIR_INST_MAY_TRAP |
                           AST_MIR_INST_VOLATILE | AST_MIR_INST_CONSTRAINT |
                           AST_MIR_INST_CONSERVATIVE_EFFECTS;
  if (!i || i->result == AST_MIR_INVALID_ID || (i->flags & effects))
    return 0;
  switch (i->op) {
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
  case AST_MIR_OP_BIND:
    return 1;
  case AST_MIR_OP_INTRINSIC:
    return i->intrinsic == AST_HIR_INTRINSIC_PTR_TO_INT ||
           i->intrinsic == AST_HIR_INTRINSIC_INT_TO_PTR ||
           i->intrinsic == AST_HIR_INTRINSIC_CLZ ||
           i->intrinsic == AST_HIR_INTRINSIC_CTZ ||
           i->intrinsic == AST_HIR_INTRINSIC_POPCNT ||
           i->intrinsic == AST_HIR_INTRINSIC_BSWAP ||
           i->intrinsic == AST_HIR_INTRINSIC_BITREV ||
           i->intrinsic == AST_HIR_INTRINSIC_ROL ||
           i->intrinsic == AST_HIR_INTRINSIC_ROR ||
           i->intrinsic == AST_HIR_INTRINSIC_ADD_CHECKED ||
           i->intrinsic == AST_HIR_INTRINSIC_SUB_CHECKED ||
           i->intrinsic == AST_HIR_INTRINSIC_MUL_CHECKED ||
           i->intrinsic == AST_HIR_INTRINSIC_ADD_WRAP ||
           i->intrinsic == AST_HIR_INTRINSIC_SUB_WRAP ||
           i->intrinsic == AST_HIR_INTRINSIC_MUL_WRAP ||
           i->intrinsic == AST_HIR_INTRINSIC_ADD_SAT ||
           i->intrinsic == AST_HIR_INTRINSIC_SUB_SAT;
  default:
    return 0;
  }
}

/* The 16/32 encoder deliberately consumes generic MIR directly, so it does
   not pass through the common Machine-IR peephole. Give it the same essential
   SSA dead-result cleanup here. The pass is fail-closed: anything with memory,
   call, volatile, trap, constraint or conservative effects remains emitted. */
static int prepare_dce(Legacy *c) {
  const uint32_t ni = c->info.instruction_count, nv = c->info.vreg_count;
  uint32_t *uses = (uint32_t *)calloc(nv ? nv : 1u, sizeof(*uses));
  c->dead_insts = (uint8_t *)calloc(ni ? ni : 1u, 1u);
  c->dead_values = (uint8_t *)calloc(nv ? nv : 1u, 1u);
  if (!uses || !c->dead_insts || !c->dead_values) {
    free(uses);
    return encoder_fail(&c->b, AST_ENCODER_DIAG_OUT_OF_MEMORY,
                        "Cannot allocate legacy DCE state.");
  }
  for (uint32_t id = 0; id < ni; ++id) {
    AstMirInstructionInfo i;
    if (!ast_mir_function_instruction(c->fn, id, &i)) {
      free(uses);
      return fail(c, "Invalid MIR instruction during legacy DCE.");
    }
    for (uint32_t n = 0; n < i.operand_count; ++n)
      if (i.operands[n] < nv)
        ++uses[i.operands[n]];
  }
  for (uint32_t b = 0; b < c->info.block_count; ++b) {
    AstMirBlockInfo block;
    if (!ast_mir_function_block(c->fn, b, &block)) {
      free(uses);
      return fail(c, "Invalid MIR block during legacy DCE.");
    }
    const AstMirTerminatorInfo *t = &block.terminator;
    if (t->condition_or_value != AST_MIR_INVALID_ID &&
        t->condition_or_value < nv)
      ++uses[t->condition_or_value];
    for (uint32_t n = 0; n < t->true_arg_count; ++n)
      if (t->true_args[n] < nv)
        ++uses[t->true_args[n]];
    for (uint32_t n = 0; n < t->false_arg_count; ++n)
      if (t->false_args[n] < nv)
        ++uses[t->false_args[n]];
  }
  int changed;
  do {
    changed = 0;
    for (uint32_t id = ni; id-- > 0u;) {
      AstMirInstructionInfo i;
      if (c->dead_insts[id] || !ast_mir_function_instruction(c->fn, id, &i) ||
          !dce_candidate(&i) || i.result >= nv || uses[i.result])
        continue;
      c->dead_insts[id] = 1u;
      c->dead_values[i.result] = 1u;
      changed = 1;
      for (uint32_t n = 0; n < i.operand_count; ++n)
        if (i.operands[n] < nv && uses[i.operands[n]])
          --uses[i.operands[n]];
    }
  } while (changed);
  free(uses);
  return 1;
}

static int reserve_home(Legacy *c, uint64_t *used, uint64_t size,
                        uint32_t alignment, int32_t *home) {
  if (!alignment || alignment > 16 || (alignment & (alignment - 1)) ||
      size > INT32_MAX || *used > INT32_MAX - size - 16)
    return fail(c, "Unsupported legacy stack object size/alignment.");
  *used =
      ((*used + size + 8u + alignment - 1u) & ~(uint64_t)(alignment - 1u)) - 8u;
  *home = -(int32_t)*used;
  return 1;
}
static int prepare_layout(Legacy *c) {
  uint32_t n = c->info.block_count;
  uint32_t *pending = calloc(n ? n : 1u, sizeof(*pending));
  uint8_t *seen = calloc(n ? n : 1u, 1u);
  c->layout = calloc(n ? n : 1u, sizeof(*c->layout));
  if (!pending || !seen || !c->layout) {
    free(pending);
    free(seen);
    return 0;
  }
  uint32_t top = 0u;
  pending[top++] = c->info.entry_block;
  seen[c->info.entry_block] = 1u;
  while (top) {
    uint32_t id = pending[--top];
    for (;;) {
      c->layout[c->layout_count++] = id;
      AstMirBlockInfo bb;
      if (!ast_mir_function_block(c->fn, id, &bb)) {
        free(pending);
        free(seen);
        return 0;
      }
      const AstMirTerminatorInfo *t = &bb.terminator;
      uint32_t preferred = AST_MIR_INVALID_ID;
      if (t->kind == AST_MIR_TERM_BR)
        preferred = t->target_true;
      else if (t->kind == AST_MIR_TERM_COND_BR) {
        AstMirBlockInfo no;
        if (!ast_mir_function_block(c->fn, t->target_false, &no)) {
          free(pending);
          free(seen);
          return 0;
        }
        int cold = no.terminator.kind == AST_MIR_TERM_RETURN ||
                   no.terminator.kind == AST_MIR_TERM_UNREACHABLE;
        preferred = cold ? t->target_true : t->target_false;
        uint32_t other = cold ? t->target_false : t->target_true;
        if (!seen[other]) {
          seen[other] = 1u;
          pending[top++] = other;
        }
      }
      if (preferred == AST_MIR_INVALID_ID || seen[preferred])
        break;
      seen[preferred] = 1u;
      id = preferred;
    }
  }
  free(pending);
  free(seen);
  return 1;
}
static void block_home_use(const Legacy *c, uint8_t *blocked, uint32_t b,
                           AstMirVReg value) {
  if (value < c->info.vreg_count &&
      c->values[value].kind == AST_MIR_VREG_INSTRUCTION_RESULT &&
      c->values[value].block != b)
    blocked[value] = 1u;
}
static int prepare_homes(Legacy *c, uint64_t *used) {
  uint32_t n = c->info.vreg_count;
  uint32_t *aliases = calloc(n ? n : 1u, sizeof(*aliases));
  uint8_t *blocked = calloc(n ? n : 1u, 1u);
  if (!aliases || !blocked) {
    free(aliases);
    free(blocked);
    return 0;
  }
  for (uint32_t v = 0; v < n; ++v) {
    aliases[v] = v;
    if (c->values[v].kind == AST_MIR_VREG_BLOCK_ARGUMENT &&
        !(c->values[v].flags & AST_MIR_VREG_FUNCTION_ARGUMENT))
      blocked[v] = 1u;
  }
  for (uint32_t b = 0; b < c->info.block_count; ++b) {
    AstMirBlockInfo bb;
    ast_mir_function_block(c->fn, b, &bb);
    for (uint32_t j = 0; j < bb.instruction_count; ++j) {
      AstMirInstructionInfo i;
      ast_mir_function_instruction(c->fn, bb.instructions[j], &i);
      if (bb.loop_depth && i.result < n)
        blocked[i.result] = 1u;
      for (uint32_t o = 0; o < i.operand_count; ++o) {
        block_home_use(c, blocked, b, i.operands[o]);
        if (i.operands[o] < n &&
            (i.op == AST_MIR_OP_ADDRESS_OF ||
             (i.flags &
              (AST_MIR_INST_VOLATILE | AST_MIR_INST_ATOMIC |
               AST_MIR_INST_CONSTRAINT | AST_MIR_INST_CONSERVATIVE_EFFECTS))))
          blocked[i.operands[o]] = 1u;
      }
    }
    block_home_use(c, blocked, b, bb.terminator.condition_or_value);
    for (uint32_t a = 0; a < bb.terminator.true_arg_count; ++a)
      block_home_use(c, blocked, b, bb.terminator.true_args[a]);
    for (uint32_t a = 0; a < bb.terminator.false_arg_count; ++a)
      block_home_use(c, blocked, b, bb.terminator.false_args[a]);
  }
  for (uint32_t j = 0; j < c->info.instruction_count; ++j) {
    AstMirInstructionInfo i;
    ast_mir_function_instruction(c->fn, j, &i);
    if (i.op != AST_MIR_OP_COPY || i.operand_count != 1u || i.result >= n ||
        i.operands[0] >= i.result || blocked[i.result] ||
        blocked[i.operands[0]] || i.effects ||
        (i.flags & ~(uint32_t)AST_MIR_INST_SYNTHETIC) ||
        c->values[i.result].type != c->values[i.operands[0]].type)
      continue;
    uint32_t root = aliases[i.operands[0]];
    if (!blocked[root])
      aliases[i.result] = root;
  }
  int ok = 1;
  for (uint32_t v = 0; v < n; ++v) {
    if (c->dead_values[v])
      continue;
    if (aliases[v] != v)
      c->homes[v] = c->homes[aliases[v]];
    else if (!reserve_home(c, used, ast_sema_type_size(c->values[v].type), 4u,
                           &c->homes[v])) {
      ok = 0;
      break;
    }
  }
  free(aliases);
  free(blocked);
  return ok;
}

static int prepare(Legacy *c) {
  if (c->info.flags & AST_HIR_FUNCTION_NAKED)
    return fail(c, "Legacy naked functions require a native entry contract.");
  const AstSemaType *result = ast_sema_type_function_result(c->info.type);
  c->has_sret = aggregate_type(result);
  if (c->has_sret) {
    uint64_t size = ast_sema_type_size(result);
    if (!size || size == UINT64_MAX || size > UINT32_MAX)
      return fail(
          c, "Legacy aggregate result has no concrete return-buffer size.");
    c->sret_size = (uint32_t)size;
  }
  size_t nv = c->info.vreg_count, ns = c->info.stack_slot_count,
         nb = c->info.block_count;
  c->homes = calloc(nv ? nv : 1, sizeof(*c->homes));
  c->values = calloc(nv ? nv : 1, sizeof(*c->values));
  c->slots = calloc(ns ? ns : 1, sizeof(*c->slots));
  c->blocks = calloc(nb ? nb : 1, sizeof(*c->blocks));
  if (!c->homes || !c->values || !c->slots || !c->blocks)
    return encoder_fail(&c->b, AST_ENCODER_DIAG_OUT_OF_MEMORY,
                        "Cannot allocate legacy stack layout.");
  uint64_t used = 0, max_edge = 0;
  for (uint32_t v = 0; v < nv; ++v) {
    ast_mir_function_vreg(c->fn, v, &c->values[v]);
    const AstSemaType *type = c->values[v].type;
    AstSemaTypeKind kind = ast_sema_type_kind(type);
    uint64_t size = ast_sema_type_size(type);
    if (kind == AST_SEMA_TYPE_VECTOR || kind == AST_SEMA_TYPE_MASK ||
        kind == AST_SEMA_TYPE_SVECTOR || kind == AST_SEMA_TYPE_PRED)
      return fail(c, "Legacy vector ABI is not yet defined.");
    if ((kind == AST_SEMA_TYPE_INT || kind == AST_SEMA_TYPE_BITS) &&
        c->values[v].bit_width != 8 && c->values[v].bit_width != 16 &&
        c->values[v].bit_width != 32 && c->values[v].bit_width != 64)
      return fail(c, "Legacy nonstandard integer width has not been lowered.");
    if ((kind == AST_SEMA_TYPE_INT || kind == AST_SEMA_TYPE_BITS ||
         kind == AST_SEMA_TYPE_FLOAT) &&
        size > 8)
      return fail(
          c, "Legacy scalar values wider than 64 bits are not yet supported.");
    if (size == UINT64_MAX || !size)
      return fail(c, "Legacy value has no concrete stack representation.");
  }
  if (!prepare_dce(c))
    return 0;
  if (!prepare_homes(c, &used) || !prepare_layout(c))
    return 0;
  for (uint32_t s = 0; s < ns; ++s) {
    AstMirStackSlotInfo slot;
    ast_mir_function_stack_slot(c->fn, s, &slot);
    if (!reserve_home(c, &used, slot.size, slot.alignment, &c->slots[s]))
      return 0;
  }
  for (uint32_t b = 0; b < nb; ++b) {
    AstMirBlockInfo block;
    ast_mir_function_block(c->fn, b, &block);
    uint64_t bytes = 0;
    for (uint32_t n = 0; n < block.argument_count; ++n)
      bytes += (value_size(c, block.arguments[n]) + 3u) & ~3u;
    if (bytes > max_edge)
      max_edge = bytes;
  }
  if (!reserve_home(c, &used, max_edge, 4, &c->edge_home))
    return 0;
  /* EBP is 8 mod 16 after PUSH EBP. Thus frame extent is 8 mod 16. */
  c->frame_size = (uint32_t)(((used + 7u) & ~UINT64_C(15)) + 8u);
  if (c->mode16 && c->frame_size > 32768)
    return fail(c, "Legacy 16-bit frame exceeds the supported segment window.");
  return 1;
}
static int emit_function(Legacy *c) {
  c->allow_home_forward = 1u;
  c->last_store_valid = 0u;
  if (!push(c, BP) || !rr(c, 0x89, BP, SP) ||
      !group_imm(c, 5, SP, c->frame_size))
    return 0;
  AstMirBlockInfo entry;
  ast_mir_function_block(c->fn, c->info.entry_block, &entry);
  uint32_t at = 8u + (c->has_sret ? 4u : 0u);
  uint32_t params = ast_sema_type_function_param_count(c->info.type);
  if (entry.argument_count < params)
    return fail(c, "Legacy entry argument count mismatch.");
  for (uint32_t n = 0; n < params; ++n) {
    AstMirVReg v = entry.arguments[n];
    uint32_t size = value_size(c, v);
    if (!copy(c, BP, c->homes[v], BP, (int32_t)at, size))
      return 0;
    at += (size + 3u) & ~3u;
  }
  for (uint32_t order = 0; order < c->layout_count; ++order) {
    uint32_t b = c->layout[order];
    AstMirBlockInfo block;
    ast_mir_function_block(c->fn, b, &block);
    c->current_block = b;
    c->next_block = order + 1u < c->layout_count ? c->layout[order + 1u]
                                                 : AST_MIR_INVALID_ID;
    c->last_store_valid = 0u;
    c->blocks[b] = c->b.out->size;
    for (uint32_t n = 0; n < block.instruction_count; ++n) {
      AstMirInstructionInfo i;
      ast_mir_function_instruction(c->fn, block.instructions[n], &i);
      c->b.instruction = i.id;
      c->allow_home_forward =
          (uint8_t)((i.flags & (AST_MIR_INST_VOLATILE | AST_MIR_INST_ATOMIC |
                                AST_MIR_INST_CONSERVATIVE_EFFECTS |
                                AST_MIR_INST_CONSTRAINT)) == 0u &&
                    i.op != AST_MIR_OP_ATOMIC && i.op != AST_MIR_OP_FENCE &&
                    i.op != AST_MIR_OP_CALL_DIRECT &&
                    i.op != AST_MIR_OP_CALL_INDIRECT);
      if (!c->allow_home_forward)
        c->last_store_valid = 0u;
      if (c->dead_insts && c->dead_insts[i.id])
        continue;
      if (!instruction(c, &i))
        return 0;
    }
    c->allow_home_forward = 1u;
    if (!terminator(c, &block.terminator))
      return 0;
  }
  return 1;
}
typedef struct LegacyJobs {
  const AstMirProgram *mir;
  const AstTargetConfig *target;
  AstEncoderModuleOutput *out;
} LegacyJobs;
static int legacy_encode_function(void *opaque, size_t f) {
  const LegacyJobs *jobs = opaque;
  Legacy c = {0};
  c.fn = ast_mir_function_at(jobs->mir, f);
  c.mode16 = jobs->target->arch == AST_TARGET_ARCH_X86_16;
  c.b.out = &jobs->out->functions[f];
  c.b.target = *jobs->target;
  c.b.out->diagnostic = AST_ENCODER_DIAG_NONE;
  if (!ast_mir_function_info(c.fn, &c.info))
    return 0;
  if (c.info.flags & AST_HIR_FUNCTION_EXTERN)
    return 1;
  int ok = prepare(&c);
  for (unsigned pass = 0; ok && pass < 2; ++pass) {
    c.b.block_offsets = pass ? c.blocks : NULL;
    c.b.out->size = c.b.out->fixup_count = 0;
    ok = emit_function(&c);
  }
  free(c.homes);
  free(c.values);
  free(c.slots);
  free(c.blocks);
  free(c.dead_insts);
  free(c.dead_values);
  free(c.layout);
  if (!ok && c.b.out->diagnostic == AST_ENCODER_DIAG_NONE)
    encoder_fail(&c.b, AST_ENCODER_DIAG_OUT_OF_MEMORY,
                 "Cannot encode legacy function.");
  return ok;
}
int ast_encoder_x86_legacy_module(const AstMirProgram *mir,
                                  const AstTargetConfig *target,
                                  AstEncoderModuleOutput *out) {
  if (!out)
    return 0;
  memset(out, 0, sizeof(*out));
  out->failed_function = SIZE_MAX;
  if (!mir || !target ||
      (target->arch != AST_TARGET_ARCH_X86_16 &&
       target->arch != AST_TARGET_ARCH_X86_32) ||
      target->pointer_bits != 32) {
    out->diagnostic = AST_ENCODER_DIAG_INVALID_ARGUMENT;
    out->message = "Legacy x86 requires a 32-bit pointer model.";
    return 0;
  }
  size_t count = ast_mir_function_count(mir);
  out->functions = calloc(count ? count : 1, sizeof(*out->functions));
  if (!out->functions) {
    out->diagnostic = AST_ENCODER_DIAG_OUT_OF_MEMORY;
    return 0;
  }
  out->function_count = count;
  LegacyJobs jobs = {mir, target, out};
  size_t failed = SIZE_MAX;
  if (!ast_parallel_check(count, 0u, legacy_encode_function, &jobs, &failed)) {
    AstEncoderDiagCode diagnostic = out->functions[failed].diagnostic;
    const char *message = out->functions[failed].message;
    ast_encoder_module_output_destroy(out);
    out->diagnostic = diagnostic;
    out->message = message;
    out->failed_function = failed;
    return 0;
  }
  out->diagnostic = AST_ENCODER_DIAG_NONE;
  return 1;
}
