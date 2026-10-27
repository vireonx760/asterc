/* SPDX-License-Identifier: MPL-2.0 */
#include "asterdump_x86.h"
#include "../isa/system_contract.h"
#include "../isa/vector_contract.h"
#include <inttypes.h>
#include <stdio.h>

static const char *const regs64[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp",
                                       "rsi", "rdi", "r8",  "r9",  "r10", "r11",
                                       "r12", "r13", "r14", "r15"};
static const char *const regs32[16] = {
    "eax", "ecx", "edx",  "ebx",  "esp",  "ebp",  "esi",  "edi",
    "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};
static const char *const regs16[16] = {
    "ax",  "cx",  "dx",   "bx",   "sp",   "bp",   "si",   "di",
    "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"};
static const char *const regs8[16] = {
    "al",  "cl",  "dl",   "bl",   "spl",  "bpl",  "sil",  "dil",
    "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"};
static const char *const conditions[16] = {"o",  "no", "b",  "ae", "e", "ne",
                                           "be", "a",  "s",  "ns", "p", "np",
                                           "l",  "ge", "le", "g"};
static const char *gpr_name(unsigned bits, unsigned reg, uint8_t rex) {
  static const char *const high8[] = {"ah", "ch", "dh", "bh"};
  if (bits == 8u)
    return !rex && reg >= 4u && reg < 8u ? high8[reg - 4u] : regs8[reg];
  return bits == 16u ? regs16[reg] : bits == 64u ? regs64[reg] : regs32[reg];
}
static uint16_t u16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8u));
}
static uint32_t u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) | ((uint32_t)p[2] << 16u) |
         ((uint32_t)p[3] << 24u);
}
static uint64_t u64(const uint8_t *p) {
  return (uint64_t)u32(p) | ((uint64_t)u32(p + 4u) << 32u);
}
static int branch_target(uint64_t pc, size_t count, int64_t delta,
                         uint64_t *to) {
  if (pc > UINT64_MAX - count)
    return 0;
  uint64_t next = pc + count;
  if (delta < 0) {
    uint64_t magnitude = (uint64_t)(-delta);
    if (next < magnitude)
      return 0;
    *to = next - magnitude;
  } else {
    if (next > UINT64_MAX - (uint64_t)delta)
      return 0;
    *to = next + (uint64_t)delta;
  }
  return 1;
}
static int x86_address(const uint8_t *p, size_t length, size_t *position,
                       uint8_t modrm, uint8_t rex, char *out, size_t capacity,
                       unsigned tuple) {
  unsigned mod = modrm >> 6u, rm = modrm & 7u;
  if (mod == 3u)
    return 0;
  int is_rip = mod == 0u && rm == 5u;
  int has_base = !is_rip;
  int has_index = 0;
  unsigned scale = 1u, base = rm | ((rex & 1u) << 3u), index = 0u;
  if (rm == 4u) {
    if (*position >= length)
      return 0;
    uint8_t sib = p[(*position)++];
    scale = 1u << (sib >> 6u);
    unsigned field = (sib >> 3u) & 7u;
    has_index = field != 4u || (rex & 2u) != 0u;
    index = field | ((rex & 2u) << 2u);
    base = (sib & 7u) | ((rex & 1u) << 3u);
    if (mod == 0u && (sib & 7u) == 5u)
      has_base = 0;
  }
  size_t disp_bytes = mod == 1u ? 1u : mod == 2u || !has_base ? 4u : 0u;
  if (length - *position < disp_bytes)
    return 0;
  int64_t displacement = 0;
  if (disp_bytes == 1u)
    displacement = (int64_t)(int8_t)p[(*position)++] * (int64_t)tuple;
  else if (disp_bytes == 4u) {
    displacement = (int32_t)u32(p + *position);
    *position += 4u;
  }
  size_t size = 0u;
  int written = snprintf(out, capacity, "[");
  if (written < 0 || (size_t)written >= capacity)
    return 0;
  size += (size_t)written;
  if (is_rip)
    written = snprintf(out + size, capacity - size, "rip");
  else if (has_base)
    written = snprintf(out + size, capacity - size, "%s", regs64[base]);
  else
    written = 0;
  if (written < 0 || (size_t)written >= capacity - size)
    return 0;
  size += (size_t)written;
  if (has_index) {
    written = snprintf(out + size, capacity - size, "%s%s*%u",
                       (has_base || is_rip) ? "+" : "", regs64[index], scale);
    if (written < 0 || (size_t)written >= capacity - size)
      return 0;
    size += (size_t)written;
  }
  if (displacement || (!has_base && !has_index && !is_rip)) {
    uint64_t absdisp =
        displacement < 0 ? (uint64_t)(-displacement) : (uint64_t)displacement;
    written = snprintf(out + size, capacity - size, "%s0x%" PRIx64,
                       displacement < 0                    ? "-"
                       : (has_base || has_index || is_rip) ? "+"
                                                           : "",
                       absdisp);
    if (written < 0 || (size_t)written >= capacity - size)
      return 0;
    size += (size_t)written;
  }
  written = snprintf(out + size, capacity - size, "]");
  return written > 0 && (size_t)written < capacity - size;
}
static int x86_operand(const uint8_t *p, size_t length, size_t *position,
                       uint8_t modrm, uint8_t rex, unsigned bits, char *out,
                       size_t capacity) {
  if ((modrm >> 6u) != 3u)
    return x86_address(p, length, position, modrm, rex, out, capacity, 1u);
  unsigned reg = (modrm & 7u) | ((rex & 1u) << 3u);
  int written = snprintf(out, capacity, "%s", gpr_name(bits, reg, rex));
  return written >= 0 && (size_t)written < capacity;
}
/* Alignment NOPs may have repeated 66/CS prefixes. Accept those prefixes
 only
 * for actual NOP forms, and enforce the architectural 15-byte limit. */
static int nop_decode(const uint8_t *p, size_t length, size_t *used) {
  size_t n = 0u;
  while (n < length && n < 15u && (p[n] == 0x66u || p[n] == 0x2eu))
    ++n;
  if (n == length || n == 15u)
    return 0;
  if (p[n++] == 0x90u) {
    *used = n;
    return 1;
  }
  if (p[n - 1u] != 0x0fu || length - n < 2u || p[n++] != 0x1fu)
    return 0;
  uint8_t modrm = p[n++];
  if ((modrm & 0x38u) || (modrm >> 6u) == 3u)
    return 0;
  char address[120];
  if (!x86_address(p, length, &n, modrm, 0u, address, sizeof(address), 1u) ||
      n > 15u)
    return 0;
  *used = n;
  return 1;
}
static int vector_decode(const uint8_t *p, size_t length, char *out,
                         size_t capacity, size_t *used) {
  size_t n = 0u;
  unsigned pp = 0u, map = 1u, source = 0u, bits = 128u;
  uint8_t rex = 0u;
  int vex = 0, evex = 0;
  unsigned upper_rm = 0u, upper_dst = 0u, mask = 0u, zero = 0u, w = 0u;
  if (!length)
    return 0;
  if (p[n] == 0x66u || p[n] == 0xf3u || p[n] == 0xf2u) {
    pp = p[n] == 0x66u ? 1u : p[n] == 0xf3u ? 2u : 3u;
    if (++n == length)
      return 0;
  }
  if (p[n] >= 0x40u && p[n] <= 0x4fu) {
    rex = p[n++];
    if (n == length || (rex & 8u) || rex == 0x40u)
      return 0;
  }
  if (p[n] == 0x62u) {
    if (pp || rex || length - n < 4u)
      return 0;
    evex = 1;
    ++n;
    unsigned p0 = p[n++], p1 = p[n++], p2 = p[n++];
    if ((p0 & 12u) || !(p1 & 4u) || (p2 & 0x10u) || (p2 & 0x60u) == 0x60u)
      return 0;
    map = p0 & 3u;
    if (!map)
      return 0;
    rex = (uint8_t)((p0 & 0x80u ? 0u : 4u) | (p0 & 0x40u ? 0u : 2u) |
                    (p0 & 0x20u ? 0u : 1u));
    upper_dst = p0 & 0x10u ? 0u : 16u;
    upper_rm = p0 & 0x40u ? 0u : 16u;
    source = ((~p1 >> 3u) & 15u) | (p2 & 8u ? 0u : 16u);
    pp = p1 & 3u;
    w = p1 >> 7u;
    bits = (p2 & 0x60u) == 0x40u ? 512u : (p2 & 0x60u) == 0x20u ? 256u : 128u;
    mask = p2 & 7u;
    zero = p2 >> 7u;
    if (zero && !mask)
      return 0;
  } else if (p[n] == 0xc5u || p[n] == 0xc4u) {
    if (pp || rex)
      return 0;
    vex = 1;
    uint8_t first = p[n++];
    if (length - n < (first == 0xc5u ? 1u : 2u))
      return 0;
    uint8_t extension = p[n++], last = extension;
    rex = (extension & 0x80u) ? 0u : 4u;
    if (first == 0xc4u) {
      map = extension & 31u;
      if (map < 1u || map > 3u)
        return 0;
      rex = (uint8_t)(rex | ((extension & 0x40u) ? 0u : 2u));
      rex = (uint8_t)(rex | ((extension & 0x20u) ? 0u : 1u));
      last = p[n++];
      if (last & 0x80u)
        return 0;
    }
    source = (~(unsigned)last >> 3u) & 15u;
    pp = last & 3u;
    bits = (last & 4u) ? 256u : 128u;
  } else {
    if (length - n < 2u || p[n++] != 0x0fu)
      return 0;
    if (p[n] == 0x38u || p[n] == 0x3au) {
      map = p[n++] == 0x38u ? 2u : 3u;
    }
  }
  if (length - n < 2u)
    return 0;
  unsigned opcode = p[n++];
  uint8_t modrm = p[n++];
  const VectorForm *form = NULL;
  int store = 0;
  for (uint32_t j = 0; j < VECTOR_COUNT; ++j) {
    const VectorForm *f = &vector_forms[j];
    if (f->arch != AST_TARGET_ARCH_X86_64 || vector_evex(f) != evex ||
        (!evex && vector_vex(f) != vex) || ((f->encoding >> 8u) & 3u) != map ||
        ((f->encoding >> 10u) & 3u) != pp)
      continue;
    if (evex && ((f->encoding >> 24u) & 1u) != w)
      continue;
    if ((f->encoding & 255u) == opcode) {
      form = f;
      break;
    }
    if (((f->encoding >> 16u) & 255u) &&
        ((f->encoding >> 16u) & 255u) == opcode) {
      form = f;
      store = 1;
      break;
    }
  }
  if (!form || (bits == 256u && !form->wide_feature) ||
      (store && ((modrm >> 6u) == 3u || zero)) ||
      ((vex || evex) && form->shape != VECTOR_X86_VBINARY &&
       form->shape != VECTOR_X86_EBINARY && source))
    return 0;
  unsigned destination = ((modrm >> 3u) & 7u) | ((rex & 4u) << 1u) | upper_dst;
  char operand[160];
  const char *kind = bits == 512u ? "zmm" : bits == 256u ? "ymm" : "xmm";
  if ((modrm >> 6u) == 3u)
    (void)snprintf(operand, sizeof(operand), "%s%u", kind,
                   (modrm & 7u) | ((rex & 1u) << 3u) | upper_rm);
  else {
    char address[128];
    if (!x86_address(p, length, &n, modrm, rex, address, sizeof(address),
                     evex ? bits / 8u : 1u))
      return 0;
    (void)snprintf(operand, sizeof(operand), "%sword ptr %s", kind, address);
  }
  if (n > 15u)
    return 0;
  int written;
  char suffix[24] = "";
  if (mask)
    (void)snprintf(suffix, sizeof(suffix), " {k%u}%s", mask,
                   zero ? " {z}" : "");
  if (store)
    written = snprintf(out, capacity, "%s %s%s, %s%u", form->name, operand,
                       suffix, kind, destination);
  else if (form->shape == VECTOR_X86_VBINARY ||
           form->shape == VECTOR_X86_EBINARY)
    written = snprintf(out, capacity, "%s %s%u%s, %s%u, %s", form->name, kind,
                       destination, suffix, kind, source, operand);
  else
    written = snprintf(out, capacity, "%s %s%u%s, %s", form->name, kind,
                       destination, suffix, operand);
  if (written < 0 || (size_t)written >= capacity)
    return 0;
  *used = n;
  return 1;
}
#define EMIT(...)                                                              \
  do {                                                                         \
    (void)snprintf(text, text_cap, __VA_ARGS__);                               \
    *consumed = n;                                                             \
    return 1;                                                                  \
  } while (0)
static int extended_decode(const uint8_t *p, size_t length, char *text,
                           size_t text_cap, size_t *consumed) {
  size_t n = 0u;
  unsigned prefix = 0u;
  int operand16 = 0;
  while (n < length &&
         (p[n] == 0x66u || p[n] == 0xf0u || p[n] == 0xf2u || p[n] == 0xf3u)) {
    uint8_t next = p[n++];
    if (next == 0x66u) {
      if (operand16)
        return 0;
      operand16 = 1;
    } else {
      if (prefix)
        return 0;
      prefix = next;
    }
  }
  if (n == length)
    return 0;
  uint8_t rex = 0u;
  if ((p[n] & 0xf0u) == 0x40u) {
    rex = p[n++];
    if (n == length)
      return 0;
  }
  unsigned bits = (rex & 8u) ? 64u : operand16 ? 16u : 32u;
  unsigned op = p[n++];
  int escaped = op == 0x0fu;
  if (escaped) {
    if (n == length)
      return 0;
    op = p[n++];
  }
  int exchange = !escaped && (op == 0x86u || op == 0x87u);
  int cmpxchg = escaped && (op == 0xb0u || op == 0xb1u);
  int xadd = escaped && (op == 0xc0u || op == 0xc1u);
  if (exchange || cmpxchg || xadd) {
    if ((prefix && prefix != 0xf0u) || n == length)
      return 0;
    if (!(op & 1u)) {
      if (operand16)
        return 0;
      bits = 8u;
    }
    uint8_t modrm = p[n++];
    if (prefix && (modrm >> 6u) == 3u)
      return 0; /* LOCK requires a memory destination. */
    unsigned reg = ((modrm >> 3u) & 7u) | ((rex & 4u) << 1u);
    char operand[120];
    if (!x86_operand(p, length, &n, modrm, rex, bits, operand, sizeof(operand)))
      return 0;
    EMIT("%s%s %s, %s", prefix ? "lock " : "",
         exchange  ? "xchg"
         : cmpxchg ? "cmpxchg"
                   : "xadd",
         operand, gpr_name(bits, reg, rex));
  }
  if (!escaped || n == length || prefix == 0xf0u)
    return 0;
  uint8_t modrm = p[n++];
  unsigned reg = ((modrm >> 3u) & 7u) | ((rex & 4u) << 1u);
  if (op == 0xb8u && prefix == 0xf3u) {
    char operand[120];
    if (!x86_operand(p, length, &n, modrm, rex, bits, operand, sizeof(operand)))
      return 0;
    EMIT("popcnt %s, %s", gpr_name(bits, reg, rex), operand);
  }
  if (op == 0xbau && !prefix) {
    unsigned extension = (modrm >> 3u) & 7u;
    static const char *const names[] = {"bt", "bts", "btr", "btc"};
    char operand[120];
    if (extension < 4u ||
        !x86_operand(p, length, &n, modrm, rex, bits, operand,
                     sizeof(operand)) ||
        n == length)
      return 0;
    unsigned bit = p[n++];
    EMIT("%s %s, %u", names[extension - 4u], operand, bit);
  }
  /* Scalar SSE uses mandatory prefixes, not a 16-bit GPR width. */
  int scalar = prefix == 0xf2u || prefix == 0xf3u;
  int move = scalar && !operand16 && (op == 0x10u || op == 0x11u);
  int arithmetic = scalar && !operand16 &&
                   (op == 0x58u || op == 0x59u || op == 0x5cu || op == 0x5eu);
  int compare = op == 0x2eu && !prefix;
  int from_integer = scalar && !operand16 && op == 0x2au;
  int to_integer = scalar && !operand16 && op == 0x2cu;
  int convert = scalar && !operand16 && op == 0x5au;
  int bit_move = !prefix && operand16 && (op == 0x6eu || op == 0x7eu);
  int wide_move = prefix == 0xf3u && !operand16 && (op == 0x6fu || op == 0x7fu);
  if (!move && !arithmetic && !compare && !from_integer && !to_integer &&
      !convert && !bit_move && !wide_move)
    return 0;
  if ((rex & 8u) && !from_integer && !to_integer && !bit_move)
    return 0;
  unsigned fp_bits = wide_move         ? 128u
                     : compare         ? (operand16 ? 64u : 32u)
                     : prefix == 0xf2u ? 64u
                                       : 32u;
  int integer_operand = from_integer || bit_move;
  unsigned operand_bits = integer_operand ? ((rex & 8u) ? 64u : 32u) : fp_bits;
  char operand[120], destination[32];
  if (integer_operand) {
    if ((modrm >> 6u) == 3u) {
      if (!x86_operand(p, length, &n, modrm, rex, operand_bits, operand,
                       sizeof(operand)))
        return 0;
    } else {
      char address[100];
      if (!x86_address(p, length, &n, modrm, rex, address, sizeof(address), 1u))
        return 0;
      (void)snprintf(operand, sizeof(operand), "%s ptr %s",
                     operand_bits == 64u ? "qword" : "dword", address);
    }
  } else if ((modrm >> 6u) == 3u) {
    unsigned rm = (modrm & 7u) | ((rex & 1u) << 3u);
    (void)snprintf(operand, sizeof(operand), "xmm%u", rm);
  } else {
    char address[100];
    if (!x86_address(p, length, &n, modrm, rex, address, sizeof(address), 1u))
      return 0;
    (void)snprintf(operand, sizeof(operand), "%s ptr %s",
                   operand_bits == 128u  ? "xmmword"
                   : operand_bits == 64u ? "qword"
                                         : "dword",
                   address);
  }
  if (to_integer)
    (void)snprintf(destination, sizeof(destination), "%s",
                   gpr_name(bits, reg, rex));
  else
    (void)snprintf(destination, sizeof(destination), "xmm%u", reg);
  const char *name;
  if (bit_move)
    name = (rex & 8u) ? "movq" : "movd";
  else if (wide_move)
    name = "movdqu";
  else if (move)
    name = fp_bits == 64u ? "movsd" : "movss";
  else if (compare)
    name = fp_bits == 64u ? "ucomisd" : "ucomiss";
  else if (from_integer)
    name = fp_bits == 64u ? "cvtsi2sd" : "cvtsi2ss";
  else if (to_integer)
    name = fp_bits == 64u ? "cvttsd2si" : "cvttss2si";
  else if (convert)
    name = fp_bits == 64u ? "cvtsd2ss" : "cvtss2sd";
  else {
    unsigned index = op == 0x58u   ? 0u
                     : op == 0x59u ? 1u
                     : op == 0x5cu ? 2u
                                   : 3u;
    static const char *const names32[] = {"addss", "mulss", "subss", "divss"};
    static const char *const names64[] = {"addsd", "mulsd", "subsd", "divsd"};
    name = fp_bits == 64u ? names64[index] : names32[index];
  }
  int store = (move && op == 0x11u) || (bit_move && op == 0x7eu) ||
              (wide_move && op == 0x7fu);
  EMIT("%s %s, %s", name, store ? operand : destination,
       store ? destination : operand);
}
int dump_x86_decode(const uint8_t *p, size_t length, uint64_t pc, char *text,
                    size_t text_cap, size_t *consumed) {
  *consumed = 1u;
  if (!p || !length || !text || !text_cap)
    return 0;
  size_t n = 0u;
  if (nop_decode(p, length, &n))
    EMIT("nop");
  if (vector_decode(p, length, text, text_cap, consumed))
    return 1;
  if (extended_decode(p, length, text, text_cap, consumed))
    return 1;
  for (uint32_t j = 0; j < SYSTEM_COUNT; ++j) {
    const SystemForm *f = &system_forms[j];
    if (f->arch != AST_TARGET_ARCH_X86_64 ||
        (f->shape != SYSTEM_SHAPE_FIXED && f->shape != SYSTEM_SHAPE_IMPLICIT))
      continue;
    size_t size = f->encoding > 0xffffu ? 3u : 2u;
    if (length < size)
      continue;
    size_t k;
    for (k = 0; k < size; ++k)
      if (p[k] != ((f->encoding >> (8u * (unsigned)k)) & 255u))
        break;
    if (k == size) {
      n = size;
      EMIT("%s", f->name);
    }
  }
  int operand16 = p[0] == 0x66u;
  if (operand16 && ++n == length)
    return 0;
  uint8_t rex = 0u;
  /* Exactly one REX prefix is supported; multiple prefixes and legacy
     prefixes are deliberately unknown rather than silently ignored. */
  if (p[n] >= 0x40u && p[n] <= 0x4fu) {
    rex = p[n++];
    if (n == length || (p[n] >= 0x40u && p[n] <= 0x4fu))
      return 0;
  }
  uint8_t op = p[n++];
  if (op >= 0xe4u && op <= 0xefu && (op <= 0xe7u || op >= 0xecu)) {
    if (rex)
      return 0;
    int input = (op & 2u) == 0u;
    const char *acc = (op & 1u) ? (operand16 ? "ax" : "eax") : "al";
    if (!(op & 1u) && operand16)
      return 0;
    if (op <= 0xe7u) {
      if (n == length)
        return 0;
      unsigned port = p[n++];
      if (input)
        EMIT("in %s, 0x%x", acc, port);
      EMIT("out 0x%x, %s", port, acc);
    }
    if (input)
      EMIT("in %s, dx", acc);
    EMIT("out dx, %s", acc);
  }
  if (op == 0xfau && !rex && !operand16)
    EMIT("cli");
  if (op == 0xfbu && !rex && !operand16)
    EMIT("sti");
  if (op == 0xf4u && !rex && !operand16)
    EMIT("hlt");
  if (op == 0xfcu && !rex && !operand16)
    EMIT("cld");
  if (op == 0x90u && !rex && !operand16)
    EMIT("nop");
  if (op == 0xc3u && !rex && !operand16)
    EMIT("ret");
  if (op == 0xccu && !rex && !operand16)
    EMIT("int3");
  if (op == 0x99u && (!rex || rex == 0x48u))
    EMIT("%s", (rex & 8u) ? "cqo" : operand16 ? "cwd" : "cdq");
  if ((op & 0xf0u) == 0x50u && (!rex || rex == 0x41u)) {
    unsigned reg = (op & 7u) | ((rex & 1u) << 3u);
    if (op < 0x58u)
      EMIT("push %s", operand16 ? regs16[reg] : regs64[reg]);
    EMIT("pop %s", operand16 ? regs16[reg] : regs64[reg]);
  }
  if ((op & 0xf8u) == 0xb0u) {
    if (operand16 || n == length)
      return 0;
    unsigned reg = (op & 7u) | ((rex & 1u) << 3u);
    unsigned imm = p[n++];
    EMIT("mov %s, 0x%x", gpr_name(8u, reg, rex), imm);
  }
  if ((op & 0xf8u) == 0xb8u) {
    unsigned reg = (op & 7u) | ((rex & 1u) << 3u);
    if (rex & 8u) {
      if (length - n < 8u)
        return 0;
      uint64_t imm = u64(p + n);
      n += 8u;
      EMIT("mov %s, 0x%" PRIx64, regs64[reg], imm);
    }
    if (operand16) {
      if (length - n < 2u)
        return 0;
      unsigned imm = u16(p + n);
      n += 2u;
      EMIT("mov %s, 0x%x", regs16[reg], imm);
    }
    if (length - n < 4u)
      return 0;
    uint32_t imm = u32(p + n);
    n += 4u;
    EMIT("mov %s, 0x%08" PRIx32, regs32[reg], imm);
  }
  if (op == 0xe8u || op == 0xe9u || op == 0xebu || (op & 0xf0u) == 0x70u) {
    size_t bytes = op == 0xe8u || op == 0xe9u ? 4u : 1u;
    if (rex || operand16 || length - n < bytes)
      return 0;
    int64_t diff =
        bytes == 4u ? (int64_t)(int32_t)u32(p + n) : (int64_t)(int8_t)p[n];
    n += bytes;
    uint64_t to;
    if (!branch_target(pc, n, diff, &to))
      return 0;
    if ((op & 0xf0u) == 0x70u)
      EMIT("j%s 0x%" PRIx64, conditions[op & 15u], to);
    EMIT("%s 0x%" PRIx64, op == 0xe8u ? "call" : "jmp", to);
  }
  int two_byte = 0;
  if (op == 0x0fu) {
    two_byte = 1;
    if (length == n)
      return 0;
    unsigned second = p[n++];
    if (second == 0x0bu && !rex && !operand16)
      EMIT("ud2");
    if (second >= 0x20u && second <= 0x23u) {
      if (operand16 || n == length || (rex & 10u))
        return 0;
      uint8_t modrm = p[n++];
      if ((modrm & 0xc0u) != 0xc0u)
        return 0;
      unsigned special = ((modrm >> 3u) & 7u) | ((rex & 4u) << 1u);
      unsigned gpr_index = (modrm & 7u) | ((rex & 1u) << 3u);
      int debug = (second & 1u) != 0u, writing = (second & 2u) != 0u;
      const SystemForm *f =
          &system_forms[debug ? SYSTEM_X86_READ_DR : SYSTEM_X86_READ_CR];
      if (!system_control_number(f, special))
        return 0;
      if (writing)
        EMIT("mov %sr%u, %s", debug ? "d" : "c", special, regs64[gpr_index]);
      EMIT("mov %s, %sr%u", regs64[gpr_index], debug ? "d" : "c", special);
    }
    if (second == 0x00u) {
      if (n == length || (rex & 14u))
        return 0;
      uint8_t modrm = p[n++];
      unsigned ext = (modrm >> 3u) & 7u;
      if ((modrm & 0xc0u) != 0xc0u || ext > 3u)
        return 0;
      if (operand16 && ext >= 2u)
        return 0; /* redundant prefix has no canonical text round trip */
      unsigned r = (modrm & 7u) | ((rex & 1u) << 3u);
      static const char *const names[] = {"sldt", "str", "lldt", "ltr"};
      static const char *const word_regs[] = {
          "ax",  "cx",  "dx",   "bx",   "sp",   "bp",   "si",   "di",
          "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"};
      /* SLDT/STR without 66 write r32; LLDT/LTR always read r16. */
      EMIT("%s %s", names[ext],
           (operand16 || ext >= 2u) ? word_regs[r] : regs32[r]);
    }
    if (second == 0x01u) {
      if (operand16 || n == length || (rex & 14u))
        return 0;
      uint8_t modrm = p[n++];
      unsigned ext = (modrm >> 3u) & 7u;
      if ((modrm & 0xc0u) == 0xc0u || (ext > 3u && ext != 7u))
        return 0;
      char address[120];
      if (!x86_address(p, length, &n, modrm, rex, address, sizeof(address), 1u))
        return 0;
      static const char *const names[] = {"sgdt", "sidt", "lgdt", "lidt"};
      EMIT("%s %s", ext == 7u ? "invlpg" : names[ext], address);
    }
    if ((second & 0xf0u) == 0x80u) {
      if (rex || operand16 || length - n < 4u)
        return 0;
      int64_t diff = (int32_t)u32(p + n);
      n += 4u;
      uint64_t to;
      if (!branch_target(pc, n, diff, &to))
        return 0;
      EMIT("j%s 0x%" PRIx64, conditions[second & 15u], to);
    }
    unsigned bits = (rex & 8u) ? 64u : operand16 ? 16u : 32u;
    if ((second & 0xf0u) == 0x40u || (second & 0xf0u) == 0x90u ||
        second == 0xb6u || second == 0xb7u || second == 0xbeu ||
        second == 0xbfu) {
      if (n == length)
        return 0;
      uint8_t modrm = p[n++];
      int setcc = (second & 0xf0u) == 0x90u;
      int cmov = (second & 0xf0u) == 0x40u;
      if (setcc && (operand16 || (modrm & 0x38u)))
        return 0;
      unsigned source_bits = cmov ? bits : setcc || !(second & 1u) ? 8u : 16u;
      unsigned reg = ((modrm >> 3u) & 7u) | ((rex & 4u) << 1u);
      char operand[120];
      if (!x86_operand(p, length, &n, modrm, rex, source_bits, operand,
                       sizeof(operand)))
        return 0;
      if (setcc)
        EMIT("set%s %s", conditions[second & 15u], operand);
      if (cmov)
        EMIT("cmov%s %s, %s", conditions[second & 15u],
             gpr_name(bits, reg, rex), operand);
      if (bits <= source_bits)
        return 0;
      if ((modrm >> 6u) != 3u) {
        char address[120];
        (void)snprintf(address, sizeof(address), "%s", operand);
        EMIT("%s %s, %s ptr %s", second & 8u ? "movsx" : "movzx",
             gpr_name(bits, reg, rex), source_bits == 8u ? "byte" : "word",
             address);
      }
      EMIT("%s %s, %s", second & 8u ? "movsx" : "movzx",
           gpr_name(bits, reg, rex), operand);
    }
    if (second >= 0xc8u && second <= 0xcfu) {
      /* BSWAP is defined on r32 and r64, with REX.B extending the
         register number. Never report a 16-bit encoding as valid. */
      if (operand16)
        return 0;
      unsigned reg = (second - 0xc8u) | ((rex & 1u) << 3u);
      EMIT("bswap %s", (rex & 8u) ? regs64[reg] : regs32[reg]);
    }
    if (second != 0xafu && second != 0xbcu && second != 0xbdu)
      return 0;
    op = (uint8_t)second; /* IMUL / BSF / BSR via ModRM below */
  }
  /* Decode the canonical 64-bit addressing mode from ModRM and optional SIB.
 All displacement reads are bounds checked; RIP-relative is never confused
 with absolute SIB (base=5,mod=0). Address-size overrides are unsupported. */
  unsigned bits = (rex & 8u) ? 64u : operand16 ? 16u : 32u;
  const char *const *regs = bits == 64u   ? regs64
                            : bits == 16u ? regs16
                                          : regs32;
  if (op == 0x8du) { /* LEA is meaningful only with a memory operand. */
    if (n == length)
      return 0;
    uint8_t modrm = p[n++];
    if ((modrm & 0xc0u) == 0xc0u)
      return 0;
    unsigned r = ((modrm >> 3u) & 7u) | ((rex & 4u) << 1u);
    char address[120];
    if (!x86_address(p, length, &n, modrm, rex, address, sizeof(address), 1u))
      return 0;
    EMIT("lea %s, %s", regs[r], address);
  }
  const char *operation = NULL;
  int swap = 0, same = 0;
  switch (op) {
  case 0x88u:
  case 0x89u:
    operation = "mov";
    break;
  case 0x8au:
  case 0x8bu:
    operation = "mov";
    swap = 1;
    break;
  case 0x00u:
  case 0x01u:
    operation = "add";
    break;
  case 0x02u:
  case 0x03u:
    operation = "add";
    swap = 1;
    break;
  case 0x28u:
  case 0x29u:
    operation = "sub";
    break;
  case 0x2au:
  case 0x2bu:
    operation = "sub";
    swap = 1;
    break;
  case 0x20u:
  case 0x21u:
    operation = "and";
    break;
  case 0x22u:
  case 0x23u:
    operation = "and";
    swap = 1;
    break;
  case 0x08u:
  case 0x09u:
    operation = "or";
    break;
  case 0x0au:
  case 0x0bu:
    operation = "or";
    swap = 1;
    break;
  case 0x30u:
  case 0x31u:
    operation = "xor";
    break;
  case 0x32u:
  case 0x33u:
    operation = "xor";
    swap = 1;
    break;
  case 0x38u:
  case 0x39u:
    operation = "cmp";
    break;
  case 0x3au:
  case 0x3bu:
    operation = "cmp";
    swap = 1;
    break;
  case 0x84u:
  case 0x85u:
    operation = "test";
    same = 1;
    break;
  case 0xafu:
    if (!two_byte)
      break;
    operation = "imul";
    swap = 1;
    break;
  case 0xbcu:
    if (!two_byte)
      break;
    operation = "bsf";
    swap = 1;
    break;
  case 0xbdu:
    if (!two_byte)
      break;
    operation = "bsr";
    swap = 1;
    break;
  default:
    break;
  }
  if (operation) {
    if (!two_byte && !(op & 1u)) {
      if (operand16)
        return 0;
      bits = 8u;
    }
    if (n == length)
      return 0;
    uint8_t modrm = p[n++];
    unsigned r = ((modrm >> 3u) & 7u) | ((rex & 4u) << 1u);
    char address[120];
    if (!x86_operand(p, length, &n, modrm, rex, bits, address, sizeof(address)))
      return 0;
    if (same)
      EMIT("%s %s, %s", operation, address, gpr_name(bits, r, rex));
    EMIT("%s %s, %s", operation, swap ? gpr_name(bits, r, rex) : address,
         swap ? address : gpr_name(bits, r, rex));
  }
  if (op == 0x63u || op == 0x69u || op == 0x6bu || op == 0xffu) {
    if (n == length)
      return 0;
    uint8_t modrm = p[n++];
    unsigned ext = (modrm >> 3u) & 7u;
    unsigned reg = ext | ((rex & 4u) << 1u);
    unsigned source_bits = op == 0x63u ? 32u : op == 0xffu ? 64u : bits;
    char operand[120];
    if (!x86_operand(p, length, &n, modrm, rex, source_bits, operand,
                     sizeof(operand)))
      return 0;
    if (op == 0x63u) {
      if (bits != 64u)
        return 0;
      if ((modrm >> 6u) != 3u)
        EMIT("movsxd %s, dword ptr %s", regs64[reg], operand);
      EMIT("movsxd %s, %s", regs64[reg], operand);
    }
    if (op == 0xffu) {
      if (operand16 || (ext != 2u && ext != 4u))
        return 0;
      EMIT("%s %s", ext == 2u ? "call" : "jmp", operand);
    }
    size_t bytes = op == 0x6bu ? 1u : bits == 16u ? 2u : 4u;
    if (length - n < bytes)
      return 0;
    int64_t imm = bytes == 1u   ? (int8_t)p[n]
                  : bytes == 2u ? (int16_t)u16(p + n)
                                : (int32_t)u32(p + n);
    n += bytes;
    EMIT("imul %s, %s, %" PRId64, regs[reg], operand, imm);
  }
  if (op == 0xf6u || op == 0xf7u || op == 0xc0u || op == 0xc1u || op == 0xd0u ||
      op == 0xd1u || op == 0xd2u || op == 0xd3u) {
    /* Group-three MUL/IMUL/DIV/IDIV and group-two immediate shifts.
       The instruction's ModRM.reg is an opcode extension, NOT a
       destination register; the source can be a GPR or memory operand.
       Reject reserved extensions instead of inventing a decode. */
    if (n == length)
      return 0;
    uint8_t modrm = p[n++];
    unsigned extension = (modrm >> 3u) & 7u;
    static const char *const group3[] = {NULL,  NULL,   "not", "neg",
                                         "mul", "imul", "div", "idiv"};
    static const char *const shifts[] = {"rol", "ror", NULL, NULL,
                                         "shl", "shr", NULL, "sar"};
    int group_three = op == 0xf6u || op == 0xf7u;
    const char *name = group_three ? group3[extension] : shifts[extension];
    if (!name)
      return 0;
    if (!(op & 1u)) {
      if (operand16)
        return 0;
      bits = 8u;
    }
    char address[120];
    if (!x86_operand(p, length, &n, modrm, rex, bits, address, sizeof(address)))
      return 0;
    if (group_three)
      EMIT("%s %s", name, address);
    if (op == 0xd0u || op == 0xd1u)
      EMIT("%s %s, 1", name, address);
    if (op == 0xd2u || op == 0xd3u)
      EMIT("%s %s, cl", name, address);
    if (n == length)
      return 0;
    unsigned immediate_count = p[n++];
    EMIT("%s %s, %u", name, address, immediate_count);
  }
  if (op == 0x80u || op == 0x83u || op == 0x81u || op == 0xc6u || op == 0xc7u) {
    if (n == length)
      return 0;
    uint8_t modrm = p[n++];
    unsigned subop = (modrm >> 3u) & 7u;
    static const char *const operations[] = {"add", "or",  NULL,  NULL,
                                             "and", "sub", "xor", "cmp"};
    int move = op == 0xc6u || op == 0xc7u;
    if (move && subop != 0u)
      return 0;
    if (!move && !operations[subop])
      return 0;
    if (op == 0x80u || op == 0xc6u) {
      if (operand16)
        return 0;
      bits = 8u;
    }
    char address[120];
    if (!x86_operand(p, length, &n, modrm, rex, bits, address, sizeof(address)))
      return 0;
    size_t bytes = op == 0x83u || bits == 8u ? 1u : bits == 16u ? 2u : 4u;
    if (length - n < bytes)
      return 0;
    int64_t imm = bytes == 1u   ? (int8_t)p[n]
                  : bytes == 2u ? (int16_t)u16(p + n)
                                : (int32_t)u32(p + n);
    n += bytes;
    EMIT("%s %s, #%" PRId64, move ? "mov" : operations[subop], address, imm);
  }
  return 0;
}
