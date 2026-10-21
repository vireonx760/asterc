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
int dump_x86_decode(const uint8_t *p, size_t length, uint64_t pc, char *text,
                    size_t text_cap, size_t *consumed) {
  *consumed = 1u;
  if (!p || !length || !text || !text_cap)
    return 0;
  if (vector_decode(p, length, text, text_cap, consumed))
    return 1;
  size_t n = 0u;
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
  if (operand16 && op != 0x0fu)
    return 0;
  if (op == 0xfau && !rex)
    EMIT("cli");
  if (op == 0xfbu && !rex)
    EMIT("sti");
  if (op == 0xf4u && !rex)
    EMIT("hlt");
  if (op == 0xfcu && !rex)
    EMIT("cld");
  if (op == 0x90u && !rex)
    EMIT("nop");
  if (op == 0xc3u && !rex)
    EMIT("ret");
  if (op == 0xccu && !rex)
    EMIT("int3");
  if (op == 0xf4u && !rex)
    EMIT("hlt");
  if (op == 0xfau && !rex)
    EMIT("cli");
  if (op == 0xfbu && !rex)
    EMIT("sti");
  if (op == 0xfcu && !rex)
    EMIT("cld");
  if ((op & 0xf0u) == 0x50u && (!rex || rex == 0x41u)) {
    unsigned reg = (op & 7u) | ((rex & 1u) << 3u);
    if (op < 0x58u)
      EMIT("push %s", regs64[reg]);
    EMIT("pop %s", regs64[reg]);
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
    if (length - n < 4u)
      return 0;
    uint32_t imm = u32(p + n);
    n += 4u;
    EMIT("mov %s, 0x%08" PRIx32, regs32[reg], imm);
  }
  if (op == 0xe8u || op == 0xe9u || op == 0xebu || op == 0x74u || op == 0x75u) {
    size_t bytes = op == 0xe8u || op == 0xe9u ? 4u : 1u;
    if (rex || length - n < bytes)
      return 0;
    int64_t diff =
        bytes == 4u ? (int64_t)(int32_t)u32(p + n) : (int64_t)(int8_t)p[n];
    n += bytes;
    uint64_t to;
    if (!branch_target(pc, n, diff, &to))
      return 0;
    EMIT("%s 0x%" PRIx64,
         op == 0xe8u                  ? "call"
         : op == 0xe9u || op == 0xebu ? "jmp"
         : op == 0x74u                ? "je"
                                      : "jne",
         to);
  }
  if (op == 0x0fu) {
    if (length == n)
      return 0;
    unsigned second = p[n++];
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
    if (operand16)
      return 0;
    if (second == 0x01u) {
      if (n == length || (rex & 14u))
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
    if (second == 0x84u || second == 0x85u) {
      if (rex || length - n < 4u)
        return 0;
      int64_t diff = (int32_t)u32(p + n);
      n += 4u;
      uint64_t to;
      if (!branch_target(pc, n, diff, &to))
        return 0;
      EMIT("%s 0x%" PRIx64, second == 0x84u ? "je" : "jne", to);
    }
    if (second >= 0xc8u && second <= 0xcfu) {
      /* BSWAP is defined on r32 and r64, with REX.B extending the
         register number. Never report a 16-bit encoding as valid. */
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
  const int bits = (rex & 8u) ? 64 : 32;
  const char *const *regs = bits == 64 ? regs64 : regs32;
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
  case 0x89u:
    operation = "mov";
    break;
  case 0x8bu:
    operation = "mov";
    swap = 1;
    break;
  case 0x01u:
    operation = "add";
    break;
  case 0x03u:
    operation = "add";
    swap = 1;
    break;
  case 0x29u:
    operation = "sub";
    break;
  case 0x2bu:
    operation = "sub";
    swap = 1;
    break;
  case 0x21u:
    operation = "and";
    break;
  case 0x23u:
    operation = "and";
    swap = 1;
    break;
  case 0x09u:
    operation = "or";
    break;
  case 0x0bu:
    operation = "or";
    swap = 1;
    break;
  case 0x31u:
    operation = "xor";
    break;
  case 0x33u:
    operation = "xor";
    swap = 1;
    break;
  case 0x39u:
    operation = "cmp";
    break;
  case 0x3bu:
    operation = "cmp";
    swap = 1;
    break;
  case 0x85u:
    operation = "test";
    same = 1;
    break;
  case 0xafu:
    operation = "imul";
    swap = 1;
    break;
  case 0xbcu:
    operation = "bsf";
    swap = 1;
    break;
  case 0xbdu:
    operation = "bsr";
    swap = 1;
    break;
  default:
    break;
  }
  if (operation) {
    if (n == length)
      return 0;
    uint8_t modrm = p[n++];
    unsigned r = ((modrm >> 3u) & 7u) | ((rex & 4u) << 1u);
    unsigned m = (modrm & 7u) | ((rex & 1u) << 3u);
    char address[120];
    const char *right = NULL;
    if ((modrm & 0xc0u) == 0xc0u)
      right = regs[m];
    else if (x86_address(p, length, &n, modrm, rex, address, sizeof(address),
                         1u))
      right = address;
    else
      return 0;
    if (same)
      EMIT("%s %s, %s", operation, right, regs[r]);
    EMIT("%s %s, %s", operation, swap ? regs[r] : right,
         swap ? right : regs[r]);
  }
  if (op == 0xf7u || op == 0xc1u) {
    /* Group-three MUL/IMUL/DIV/IDIV and group-two immediate shifts.
       The instruction's ModRM.reg is an opcode extension, NOT a
       destination register; the source can be a GPR or memory operand.
       Reject reserved extensions instead of inventing a decode. */
    if (n == length)
      return 0;
    uint8_t modrm = p[n++];
    unsigned extension = (modrm >> 3u) & 7u;
    static const char *const group3[] = {NULL,  NULL,   NULL,  NULL,
                                         "mul", "imul", "div", "idiv"};
    static const char *const shifts[] = {NULL,  NULL,  NULL, NULL,
                                         "shl", "shr", NULL, "sar"};
    const char *name = op == 0xf7u ? group3[extension] : shifts[extension];
    if (!name)
      return 0;
    unsigned rm = (modrm & 7u) | ((rex & 1u) << 3u);
    char address[120];
    const char *operand;
    if ((modrm & 0xc0u) == 0xc0u)
      operand = regs[rm];
    else if (x86_address(p, length, &n, modrm, rex, address, sizeof(address),
                         1u))
      operand = address;
    else
      return 0;
    if (op == 0xf7u)
      EMIT("%s %s", name, operand);
    if (n == length)
      return 0;
    unsigned immediate_count = p[n++];
    EMIT("%s %s, %u", name, operand, immediate_count);
  }
  if (op == 0x83u || op == 0x81u || op == 0xc7u) {
    if (n == length)
      return 0;
    uint8_t modrm = p[n++];
    unsigned subop = (modrm >> 3u) & 7u;
    static const char *const operations[] = {"add", "or",  NULL,  NULL,
                                             "and", "sub", "xor", "cmp"};
    if (op == 0xc7u && subop != 0u)
      return 0;
    if (op != 0xc7u && !operations[subop])
      return 0;
    unsigned m = (modrm & 7u) | ((rex & 1u) << 3u);
    char address[120];
    const char *reg = NULL;
    if ((modrm & 0xc0u) == 0xc0u)
      reg = regs[m];
    else if (x86_address(p, length, &n, modrm, rex, address, sizeof(address),
                         1u))
      reg = address;
    else
      return 0;
    size_t bytes = op == 0x83u ? 1u : 4u;
    if (length - n < bytes)
      return 0;
    int64_t imm = bytes == 1u ? (int8_t)p[n] : (int32_t)u32(p + n);
    n += bytes;
    EMIT("%s %s, #%" PRId64, op == 0xc7u ? "mov" : operations[subop], reg, imm);
  }
  return 0;
}
